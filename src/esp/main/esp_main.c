/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
#include <string.h>
#include <stdatomic.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_psram.h"
#include "esp_partition.h"
#include "driver/uart.h"
#include "esp_vfs.h"
#include "esp_system.h"

#include "ini.h"
#include "pc.h"
#include "common.h"
#include "startup_splash.h"

//
#include "esp_private/system_internal.h"
/* ne2000 network disabled: provide null send hook so ne2000.c links */
void (*_Atomic esp32_send_packet)(uint8_t *buf, int size) = NULL;

uint32_t get_uticks()
{
	return esp_system_get_time();
}

void *psmalloc(long size);
void *fbmalloc(long size);
void *bigmalloc(size_t size)
{
	return psmalloc(size);
}

static char *pcram;
static long pcram_off;
static long pcram_len;
void *pcmalloc(long size)
{
	void *ret = pcram + pcram_off;

	size = (size + 31) / 32 * 32;
	if (pcram_off + size > pcram_len) {
		fprintf(stderr, "pcram error %ld %ld %ld\n", size, pcram_off, pcram_len);
		abort();
	}
	pcram_off += size;
	return ret;
}

void pcmalloc_init(void *ptr, long len)
{
	pcram = ptr;
	pcram_len = len;
}

/*
 * Copy a flash partition to dst via esp_partition_mmap (MMU/D-cache path).
 *
 * Why mmap instead of esp_partition_read:
 *   esp_partition_read uses the direct-SPI / DMA path which calls
 *   spi_flash_disable_interrupts_caches_and_other_cpu() to halt the other
 *   CPU's cache during the transfer.  On ESP32-S3 N16R8 with
 *   CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y, Core 0's code lives in PSRAM and is
 *   accessed through the D-cache.  Halting that cache from Core 1 deadlocks
 *   both cores.
 *
 *   esp_partition_mmap maps flash pages into the CPU's virtual address space
 *   through the MMU.  Reading from the mapped VA goes through the normal
 *   D-cache/MSPI path, which the hardware arbitrates transparently against
 *   PSRAM accesses.  No CPU is ever halted.
 */
static esp_err_t partition_mmap_copy(const esp_partition_t *part,
				     uint8_t *dst, size_t len)
{
	const void *mmap_ptr;
	esp_partition_mmap_handle_t handle;
	esp_err_t ret = esp_partition_mmap(part, 0, len,
					   ESP_PARTITION_MMAP_DATA,
					   &mmap_ptr, &handle);
	if (ret != ESP_OK) {
		fprintf(stderr, "[bios] mmap failed: err=%d\n", (int)ret);
		fflush(stderr);
		return ret;
	}
	memcpy(dst, mmap_ptr, len);
	/*
	 * Keep this one-shot ROM mapping alive. On ESP-IDF 5.5 / ESP32-S3,
	 * unmapping here can assert in spi_flash_munmap() after the copy even
	 * though the data was read successfully. BIOS/VGABIOS are loaded once at
	 * boot, so retaining these small mappings is safer than touching the
	 * direct flash-read path that can stall the other core's cache.
	 */
	(void)handle;
	return ESP_OK;
}

#if 0
static void rom_sumcheck_log_and_guard(const char *part_label,
				       const esp_partition_t *part)
{
	RomSumCheck sc = rom_partition_sumcheck(part);
	if (!sc.read_ok) {
		fprintf(stderr, "[bios] partition sumcheck read failed: %s\n", part_label);
		fflush(stderr);
		abort();
	}
	fprintf(stderr,
		"%s(partition) len %d sumcheck(sum32=0x%08" PRIx32 ", status=%s)\n",
		part_label, (int)part->size, sc.sum32,
		(sc.all_zero || sc.all_ff) ? "FAIL" : "PASS");
	fflush(stderr);
	if (sc.all_zero || sc.all_ff) {
		fprintf(stderr, "[bios] FATAL: %s partition looks invalid (all 0x%02x)\n",
			part_label, sc.all_zero ? 0x00 : 0xFF);
		fflush(stderr);
		abort();
	}
	/* partition is valid — no caching needed; mmap used at load time */
}

#endif

static const char *rom_part_label_from_file(const char *file)
{
	const char *slash;

	if (!file || !file[0])
		return file;
	if (strncmp(file, "flash:", 6) == 0)
		return file + 6;
	if (file[0] == '/') {
		slash = strrchr(file, '/');
		if (slash && slash[1])
			return slash + 1;
	}
	return file;
}

static const esp_partition_t *rom_find_partition(const char *file)
{
	const char *part_label;

	if (!file || !file[0])
		return NULL;
	part_label = rom_part_label_from_file(file);
	return esp_partition_find_first(ESP_PARTITION_TYPE_ANY,
				       ESP_PARTITION_SUBTYPE_ANY,
				       part_label);
}

void bios_rom_partitions_precheck(const char *bios, const char *vga_bios)
{
	const esp_partition_t *part;

	fprintf(stderr, "[bios] precheck partitions...\n");
	fflush(stderr);
	if (bios && bios[0]) {
		part = rom_find_partition(bios);
		if (part) {
			fprintf(stderr, "[bios] found %s partition (%d bytes)\n",
				rom_part_label_from_file(bios), (int)part->size);
			fflush(stderr);
		} else {
			fprintf(stderr, "[bios] warn: no flash partition for %s\n", bios);
			fflush(stderr);
		}
	}
	if (vga_bios && vga_bios[0]) {
		part = rom_find_partition(vga_bios);
		if (part) {
			fprintf(stderr, "[bios] found %s partition (%d bytes)\n",
				rom_part_label_from_file(vga_bios), (int)part->size);
			fflush(stderr);
		} else {
			fprintf(stderr, "[bios] warn: no flash partition for %s\n", vga_bios);
			fflush(stderr);
		}
	}
}

int load_rom(void *phys_mem, const char *file, uword addr, int backward)
{
	const char *part_label = rom_part_label_from_file(file);
	const esp_partition_t *part = rom_find_partition(file);
	uint8_t *dst = NULL;

	if (part) {
		int len = part->size;
		dst = (uint8_t *)phys_mem + (backward ? (addr - len) : addr);
		esp_err_t ret = partition_mmap_copy(part, dst, len);
		if (ret != ESP_OK) {
			fprintf(stderr, "[bios] partition load failed: %s, err=%d\n",
				part_label, (int)ret);
			abort();
		}
		fprintf(stderr, "%s(partition) loaded %d bytes\n", part_label, len);
		return len;
	}

	if (file && file[0] == '/') {
		FILE *fp = fopen(file, "rb");
		assert(fp);
		fseek(fp, 0, SEEK_END);
		int len = ftell(fp);
		fprintf(stderr, "%s len %d\n", file, len);
		rewind(fp);
		if (backward)
			fread(phys_mem + addr - len, 1, len, fp);
		else
			fread(phys_mem + addr, 1, len, fp);
		fclose(fp);
		return len;
	}
	part = rom_find_partition(file);
	assert(part);
	{
		int len = part->size;
		dst = (uint8_t *)phys_mem + (backward ? (addr - len) : addr);
		esp_err_t ret = partition_mmap_copy(part, dst, len);
		if (ret != ESP_OK) {
			fprintf(stderr, "[bios] partition load failed: %s, err=%d\n",
				file, (int)ret);
			abort();
		}
		fprintf(stderr, "%s(partition) loaded %d bytes\n", file, len);
		return len;
	}
}

//
EventGroupHandle_t global_event_group;
struct Globals globals;
static const char *INI_PARTITION_SOURCE = "ini:partition";
static char *s_ini_partition_text;

typedef struct {
	PC *pc;
	u8 *fb1;
	u8 *fb;
} Console;

#define NN 32
Console *console_init(int width, int height)
{
	Console *c = malloc(sizeof(Console));
	c->fb1 = fbmalloc(LCD_WIDTH * LCD_HEIGHT / NN * 2);
	if (globals.panel_fb) {
		/* Zero-copy: VGA renders directly into the RGB DMA frame buffer */
		c->fb = globals.panel_fb;
	} else {
		c->fb = bigmalloc(LCD_WIDTH * LCD_HEIGHT * 2);
	}
	return c;
}

void lcd_draw(int x_start, int y_start, int x_end, int y_end, void *src);
static void redraw(void *opaque,
		   int x, int y, int w, int h)
{
	Console *s = opaque;
	for (int i = 0; i < NN; i++) {
		uint16_t *src = (uint16_t *) s->fb;
		src += LCD_WIDTH * LCD_HEIGHT / NN * i;
		memcpy(s->fb1, src, LCD_WIDTH * LCD_HEIGHT / NN * 2);
		lcd_draw(0, LCD_WIDTH / NN * i,
			 LCD_HEIGHT, LCD_WIDTH / NN * (i + 1),
			 s->fb1);
		vga_step(s->pc->vga);
		usleep(900);
	}
}

static int pc_main(const char *file)
{
	PCConfig conf;
	memset(&conf, 0, sizeof(conf));
	conf.mem_size = 8 * 1024 * 1024;
	conf.vga_mem_size = 256 * 1024;
	conf.cpu_gen = 4;
	conf.fpu = 0;

	int err;
	if (file && strcmp(file, INI_PARTITION_SOURCE) == 0) {
		err = ini_parse_string(s_ini_partition_text, parse_conf_ini, &conf);
	} else {
		err = ini_parse(file, parse_conf_ini, &conf);
	}
	if (err) {
		fprintf(stderr, "error %d\n", err);
		return err;
	}

	if (conf.width != LCD_WIDTH || conf.height != LCD_HEIGHT) {
		fprintf(stderr, "fixing width/height mismatch %dx%d => %dx%d\n",
			conf.width, conf.height, LCD_WIDTH, LCD_HEIGHT);
		conf.width = LCD_WIDTH;
		conf.height = LCD_HEIGHT;
	}

	Console *console = console_init(conf.width, conf.height);
	PC *pc = pc_new(redraw, console, console->fb, &conf);
	console->pc = pc;
	globals.pc = pc;
	globals.kbd = pc->kbd;
	globals.mouse = pc->mouse;

	load_bios_and_reset(pc);

	/* Signal vga_task: PC is fully initialised and ROMs are loaded.
	 * Doing this AFTER load_bios_and_reset avoids a Flash-PSRAM D-cache
	 * deadlock: partition_mmap_copy (Core 1, Flash MMU read) must not
	 * run concurrently with pc_vga_step (Core 0, PSRAM D-cache write). */
	xEventGroupSetBits(global_event_group, TINY386_EVENT_PC_READY);
	fprintf(stderr, "[splash] pc ready, BIOS/VGABIOS loaded\n");

	pc->boot_start_time = get_uticks();
	uint32_t step_count = 0;
	bool boot_sector_signaled = false;
	for (; pc->shutdown_state != 8;) {
		pc_step(pc);
		if (!boot_sector_signaled && pc->boot_sector_seen) {
			boot_sector_signaled = true;
			fprintf(stderr, "[splash] switching display after Booting from 0000:7c00\n");
			xEventGroupSetBits(global_event_group, TINY386_EVENT_BOOT_SECTOR);
		}
		/*
		 * Let lower-priority idle task run periodically so Task WDT
		 * does not trigger while the emulator monopolizes CPU1.
		 */
		if ((++step_count & 0x7FFu) == 0)
			vTaskDelay(1);
	}
	return 0;
}

//

void *esp_psram_get(size_t *size);
void vga_task(void *arg);
void i2s_main();
void storage_init(void);

struct esp_ini_config {
	const char *filename;
	char ssid[16];
	char pass[32];
	int enable_usb;
};

void usb_setup(void);

static void i386_task(void *arg)
{
	struct esp_ini_config *config = arg;
	int core_id = esp_cpu_get_core_id();
	fprintf(stderr, "main runs on core %d\n", core_id);
	/* Wait for LCD panel (and panel_fb) to be ready before starting the
	 * PC emulator.  console_init() uses globals.panel_fb if set. */
	xEventGroupWaitBits(global_event_group,
	                    TINY386_EVENT_LOGO_READY,
	                    pdFALSE,
	                    pdFALSE,
	                    portMAX_DELAY);
	fprintf(stderr, "[splash] logo ready, starting PC in background\n");
	pc_main(config->filename);
	vTaskDelete(NULL);
}

static char *psram;
static long psram_off;
static long psram_len;

void *psmalloc(long size)
{
	void *ret = psram + psram_off;

	size = (size + 4095) / 4096 * 4096;
	if (psram_off + size > psram_len) {
		fprintf(stderr, "psram error %ld %ld %ld\n", size, psram_off, psram_len);
		abort();
	}
	psram_off += size;
	return ret;
}

void *fbmalloc(long size)
{
	void *fb = (uint8_t *) heap_caps_calloc(1, size, MALLOC_CAP_DMA);
	if (!fb) {
		fprintf(stderr, "fbmalloc error %ld\n", size);
		abort();
	}
	return fb;
}

static int parse_ini(void* user, const char* section,
		     const char* name, const char* value)
{
	struct esp_ini_config *conf = user;
#define SEC(a) (strcmp(section, a) == 0)
#define NAME(a) (strcmp(name, a) == 0)
	if (SEC("esp")) {
		if (NAME("ssid")) {
			if (strlen(value) < 32)
				strcpy(conf->ssid, value);
		} else if (NAME("pass")) {
			if (strlen(value) < 64)
				strcpy(conf->pass, value);
		} else if (NAME("enable_usb")) {
			conf->enable_usb = atoi(value);
		}
	}
#undef SEC
#undef NAME
	return 1;
}

static int load_ini_from_partition(void)
{
	const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_ANY,
						 ESP_PARTITION_SUBTYPE_ANY,
						 "ini");
	if (!part)
		return -1;

	char *buf = malloc(part->size + 1);
	if (!buf)
		return -2;
	esp_err_t ret = esp_partition_read(part, 0, buf, part->size);
	if (ret != ESP_OK) {
		free(buf);
		return -3;
	}

	size_t len = 0;
	while (len < part->size && buf[len] != '\0' && (unsigned char)buf[len] != 0xFF)
		len++;
	while (len > 0 &&
	       (buf[len - 1] == '\r' || buf[len - 1] == '\n' ||
	        buf[len - 1] == ' ' || buf[len - 1] == '\t'))
		len--;
	if (len == 0) {
		free(buf);
		return -4;
	}

	buf[len] = '\0';
	s_ini_partition_text = buf;
	return 0;
}

void app_main(void)
{
	global_event_group = xEventGroupCreate();

#ifdef ESPDEBUG
	uart_config_t uart_config = {
		.baud_rate = 115200,
		.data_bits = UART_DATA_8_BITS,
		.parity	= UART_PARITY_DISABLE,
		.stop_bits = UART_STOP_BITS_1,
		.flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
		.source_clk = UART_SCLK_DEFAULT,
	};

	uart_param_config(UART_NUM_0, &uart_config);
	if (uart_driver_install(UART_NUM_0, 2 * 1024, 0, 0, NULL, 0) != ESP_OK) {
		assert(false);
	}
#endif

	startup_resources_mount();
	i2s_main();
	storage_init();

	esp_psram_init();
#ifndef PSRAM_ALLOC_LEN
	// use the whole psram
	size_t len;
	psram = esp_psram_get(&len);
	psram_len = len;
#else
	psram_len = PSRAM_ALLOC_LEN;
	psram = heap_caps_calloc(1, psram_len, MALLOC_CAP_SPIRAM);
#endif

	const static char *files[] = {
		"/sdcard/tiny386.ini",
		NULL,
	};
	static struct esp_ini_config config;
	if (load_ini_from_partition() == 0) {
		if (ini_parse_string(s_ini_partition_text, parse_ini, &config) == 0) {
			config.filename = INI_PARTITION_SOURCE;
			fprintf(stderr, "using ini from partition 'ini'\n");
		}
	}
	for (int i = 0; files[i]; i++) {
		if (config.filename)
			break;
		if (ini_parse(files[i], parse_ini, &config) == 0) {
			config.filename = files[i];
			break;
		}
	}
	assert(config.filename);

	{
		PCConfig bootconf;
		memset(&bootconf, 0, sizeof(bootconf));
		if (strcmp(config.filename, INI_PARTITION_SOURCE) == 0)
			ini_parse_string(s_ini_partition_text, parse_conf_ini, &bootconf);
		else
			ini_parse(config.filename, parse_conf_ini, &bootconf);
		bios_rom_partitions_precheck(
			(bootconf.bios && bootconf.bios[0]) ? bootconf.bios : "bios.bin",
			(bootconf.vga_bios && bootconf.vga_bios[0]) ? bootconf.vga_bios : "vgabios.bin");
	}

	if (config.enable_usb) {
		usb_setup();
	}

	if (psram) {
		xTaskCreatePinnedToCore(i386_task, "i386_main", 8192, &config, 3, NULL, 1);
		xTaskCreatePinnedToCore(vga_task, "vga_task", 4096, NULL, 0, NULL, 0);
	}
}
