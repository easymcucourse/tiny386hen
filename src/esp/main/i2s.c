#include <unistd.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "common.h"
#include "startup_splash.h"

static i2s_chan_handle_t                tx_chan;        // I2S tx channel handler
void mixer_callback (void *opaque, uint8_t *stream, int free);

#ifndef MIXER_BUF_LEN
#define MIXER_BUF_LEN 128
#endif

#ifndef I2S_OUTPUT_VOLUME_PERCENT
#define I2S_OUTPUT_VOLUME_PERCENT 100
#endif

#ifndef I2S_STARTUP_BEEP_HZ
#define I2S_STARTUP_BEEP_HZ 880
#endif

#ifndef I2S_STARTUP_BEEP_MS
#define I2S_STARTUP_BEEP_MS 90
#endif

#ifndef I2S_STARTUP_BEEP_VOLUME
#define I2S_STARTUP_BEEP_VOLUME 2200
#endif

static void i2s_play_startup_beep(void)
{
	int16_t buf[MIXER_BUF_LEN];
	const int sample_rate = 44100;
	const int channels = 2;
	const int frames_total = sample_rate * I2S_STARTUP_BEEP_MS / 1000;
	const int frames_per_buf = MIXER_BUF_LEN / channels;
	const int period = sample_rate / I2S_STARTUP_BEEP_HZ;
	int frame = 0;

	while (frame < frames_total) {
		size_t bwritten;
		int frames = frames_total - frame;
		if (frames > frames_per_buf)
			frames = frames_per_buf;

		for (int i = 0; i < frames; i++) {
			int phase = (frame + i) % period;
			int sample = phase < (period / 2) ? I2S_STARTUP_BEEP_VOLUME : -I2S_STARTUP_BEEP_VOLUME;
			sample = sample * I2S_OUTPUT_VOLUME_PERCENT / 100;
			buf[i * 2] = sample;
			buf[i * 2 + 1] = sample;
		}
		i2s_channel_write(tx_chan, buf, frames * channels * sizeof(int16_t),
				  &bwritten, portMAX_DELAY);
		frame += frames;
	}

	memset(buf, 0, sizeof(buf));
	for (int i = 0; i < 4; i++) {
		size_t bwritten;
		i2s_channel_write(tx_chan, buf, sizeof(buf), &bwritten, portMAX_DELAY);
	}
}

static void i2s_task(void *arg)
{
	int16_t buf[MIXER_BUF_LEN];
	int core_id = esp_cpu_get_core_id();
	fprintf(stderr, "i2s runs on core %d\n", core_id);

	esp_err_t ret = i2s_channel_enable(tx_chan);
	fprintf(stderr, "[splash] i2s enable ret=%d\n", (int)ret);
	xEventGroupWaitBits(global_event_group,
			    TINY386_EVENT_LOGO_READY,
			    pdFALSE,
			    pdFALSE,
			    portMAX_DELAY);
	fprintf(stderr, "[splash] i2s logo ready, start boot audio\n");
	if (!startup_splash_play_wav(tx_chan)) {
		fprintf(stderr, "[splash] boot music failed, playing fallback beep\n");
#ifndef TINY386_NO_STARTUP_BEEP
		i2s_play_startup_beep();
#endif
	} else {
		fprintf(stderr, "[splash] boot music played\n");
	}
	xEventGroupSetBits(global_event_group, TINY386_EVENT_AUDIO_DONE);
	fprintf(stderr, "[splash] audio done\n");

	memset(buf, 0, sizeof(buf));
	for (;;) {
		EventBits_t bits = xEventGroupWaitBits(global_event_group,
						       TINY386_EVENT_PC_READY,
						       pdFALSE,
						       pdFALSE,
						       0);
		if (bits & TINY386_EVENT_PC_READY)
			break;

		size_t bwritten;
		i2s_channel_write(tx_chan, buf, sizeof(buf), &bwritten, portMAX_DELAY);
	}

	for (;;) {
		size_t bwritten;
		memset(buf, 0, MIXER_BUF_LEN * 2);
		mixer_callback(globals.pc, (uint8_t *) buf, MIXER_BUF_LEN * 2);
		for (int i = 0; i < MIXER_BUF_LEN; i++) {
			buf[i] = buf[i] * I2S_OUTPUT_VOLUME_PERCENT / 100;
		}
		i2s_channel_write(tx_chan, buf, MIXER_BUF_LEN * 2, &bwritten, portMAX_DELAY);
	}
	i2s_channel_disable(tx_chan);
}

#ifndef I2S_NUM
#define I2S_NUM I2S_NUM_AUTO
#endif

#ifdef USE_ES8311
// adapted from: examples/peripherals/i2s/i2s_codec/i2s_es8311/main/i2s_es8311_example.c
static i2s_chan_handle_t                rx_chan;        // I2S rx channel handler (not used)
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_vol.h"
static const char *TAG = "i2s";

static esp_err_t es8311_codec_init(void)
{
	/* Initialize I2C peripheral */
	i2c_master_bus_handle_t i2c_bus_handle = NULL;
	i2c_master_bus_config_t i2c_mst_cfg = {
		.i2c_port = ES8311_I2C_NUM,
		.sda_io_num = ES8311_I2C_SDA,
		.scl_io_num = ES8311_I2C_SCL,
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.glitch_ignore_cnt = 7,
		/* Pull-up internally for no external pull-up case.
		   Suggest to use external pull-up to ensure a strong enough pull-up. */
		.flags.enable_internal_pullup = true,
	};
	ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_mst_cfg, &i2c_bus_handle));

	/* Create control interface with I2C bus handle */
	audio_codec_i2c_cfg_t i2c_cfg = {
		.port = ES8311_I2C_NUM,
		.addr = ES8311_CODEC_DEFAULT_ADDR,
		.bus_handle = i2c_bus_handle,
	};
	const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
	assert(ctrl_if);

	/* Create data interface with I2S bus handle */
	audio_codec_i2s_cfg_t i2s_cfg = {
		.port = I2S_NUM,
		.rx_handle = rx_chan,
		.tx_handle = tx_chan,
	};
	const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
	assert(data_if);

	/* Create ES8311 interface handle */
	const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
	assert(gpio_if);
	es8311_codec_cfg_t es8311_cfg = {
		.ctrl_if = ctrl_if,
		.gpio_if = gpio_if,
		.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
		.master_mode = false,
		.use_mclk = I2S_MCLK >= 0,
		.pa_pin = ES8311_PA,
		.pa_reverted = false,
		.hw_gain = {
			.pa_voltage = 5.0,
			.codec_dac_voltage = 3.3,
		},
		//.mclk_div = EXAMPLE_MCLK_MULTIPLE,
	};
	const audio_codec_if_t *es8311_if = es8311_codec_new(&es8311_cfg);
	assert(es8311_if);

	/* Create the top codec handle with ES8311 interface handle and data interface */
	esp_codec_dev_cfg_t dev_cfg = {
		.dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
		.codec_if = es8311_if,
		.data_if = data_if,
	};
	esp_codec_dev_handle_t codec_handle = esp_codec_dev_new(&dev_cfg);
	assert(codec_handle);

	/* Specify the sample configurations and open the device */
	esp_codec_dev_sample_info_t sample_cfg = {
		.bits_per_sample = I2S_DATA_BIT_WIDTH_16BIT,
		.channel = 2,
		.channel_mask = 0x03,
		.sample_rate = 44100,
	};
	if (esp_codec_dev_open(codec_handle, &sample_cfg) != ESP_CODEC_DEV_OK) {
		ESP_LOGE(TAG, "Open codec device failed");
		return ESP_FAIL;
	}

	/* Set the initial volume and gain */
	if (esp_codec_dev_set_out_vol(codec_handle, 80) != ESP_CODEC_DEV_OK) {
		ESP_LOGE(TAG, "set output volume failed");
		return ESP_FAIL;
	}
	return ESP_OK;
}
#endif

void i2s_main()
{
#ifdef I2S_MCLK
	/* Setp 1: Determine the I2S channel configuration and allocate two channels one by one
	 * The default configuration can be generated by the helper macro,
	 * it only requires the I2S controller id and I2S role
	 * The tx and rx channels here are registered on different I2S controller,
	 * Except ESP32 and ESP32-S2, others allow to register two separate tx & rx channels on a same controller */
	i2s_chan_config_t tx_chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
#ifdef USE_ES8311
	ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_chan, &rx_chan));
#else
	ESP_ERROR_CHECK(i2s_new_channel(&tx_chan_cfg, &tx_chan, NULL));
#endif
	/* Step 2: Setting the configurations of standard mode and initialize each channels one by one
	 * The slot configuration and clock configuration can be generated by the macros
	 * These two helper macros is defined in 'i2s_std.h' which can only be used in STD mode.
	 * They can help to specify the slot and clock configurations for initialization or re-configuring */
	i2s_std_config_t tx_std_cfg = {
		.clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(44100),
		.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
		.gpio_cfg = {
			.mclk = I2S_MCLK,
			.bclk = I2S_BCLK,
			.ws   = I2S_WS,
			.dout = I2S_DOUT,
			.din  = -1,
			.invert_flags = {
				.mclk_inv = false,
				.bclk_inv = false,
				.ws_inv   = false,
			},
		},
	};
	ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_chan, &tx_std_cfg));
#ifdef USE_ES8311
	ESP_ERROR_CHECK(es8311_codec_init());
#endif
	xTaskCreatePinnedToCore(i2s_task, "i2s_task", 4096, NULL, 0, NULL, 0);
#endif
}
