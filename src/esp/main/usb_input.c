#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid_usage_keyboard.h"
#include "common.h"
#include "i8042.h"

static const char *TAG = "usb_input";

typedef struct {
	hid_host_device_handle_t handle;
	hid_host_driver_event_t event;
	void *arg;
} usb_app_event_t;

static QueueHandle_t s_app_event_queue;
static bool s_usb_started;

static int hid_usage_to_linux_keycode(uint8_t hid)
{
	switch (hid) {
	case 0x04: return 30;  /* A */
	case 0x05: return 48;  /* B */
	case 0x06: return 46;  /* C */
	case 0x07: return 32;  /* D */
	case 0x08: return 18;  /* E */
	case 0x09: return 33;  /* F */
	case 0x0A: return 34;  /* G */
	case 0x0B: return 35;  /* H */
	case 0x0C: return 23;  /* I */
	case 0x0D: return 36;  /* J */
	case 0x0E: return 37;  /* K */
	case 0x0F: return 38;  /* L */
	case 0x10: return 50;  /* M */
	case 0x11: return 49;  /* N */
	case 0x12: return 24;  /* O */
	case 0x13: return 25;  /* P */
	case 0x14: return 16;  /* Q */
	case 0x15: return 19;  /* R */
	case 0x16: return 31;  /* S */
	case 0x17: return 20;  /* T */
	case 0x18: return 22;  /* U */
	case 0x19: return 47;  /* V */
	case 0x1A: return 17;  /* W */
	case 0x1B: return 45;  /* X */
	case 0x1C: return 21;  /* Y */
	case 0x1D: return 44;  /* Z */

	case 0x1E: return 2;   /* 1 */
	case 0x1F: return 3;   /* 2 */
	case 0x20: return 4;   /* 3 */
	case 0x21: return 5;   /* 4 */
	case 0x22: return 6;   /* 5 */
	case 0x23: return 7;   /* 6 */
	case 0x24: return 8;   /* 7 */
	case 0x25: return 9;   /* 8 */
	case 0x26: return 10;  /* 9 */
	case 0x27: return 11;  /* 0 */

	case 0x28: return 28;  /* ENTER */
	case 0x29: return 1;   /* ESC */
	case 0x2A: return 14;  /* BACKSPACE */
	case 0x2B: return 15;  /* TAB */
	case 0x2C: return 57;  /* SPACE */
	case 0x2D: return 12;  /* MINUS */
	case 0x2E: return 13;  /* EQUAL */
	case 0x2F: return 26;  /* LEFTBRACE */
	case 0x30: return 27;  /* RIGHTBRACE */
	case 0x31: return 43;  /* BACKSLASH */
	case 0x32: return 86;  /* NON-US */
	case 0x33: return 39;  /* SEMICOLON */
	case 0x34: return 40;  /* APOSTROPHE */
	case 0x35: return 41;  /* GRAVE */
	case 0x36: return 51;  /* COMMA */
	case 0x37: return 52;  /* DOT */
	case 0x38: return 53;  /* SLASH */
	case 0x39: return 58;  /* CAPSLOCK */

	case 0x3A: return 59;  /* F1 */
	case 0x3B: return 60;  /* F2 */
	case 0x3C: return 61;  /* F3 */
	case 0x3D: return 62;  /* F4 */
	case 0x3E: return 63;  /* F5 */
	case 0x3F: return 64;  /* F6 */
	case 0x40: return 65;  /* F7 */
	case 0x41: return 66;  /* F8 */
	case 0x42: return 67;  /* F9 */
	case 0x43: return 68;  /* F10 */
	case 0x44: return 87;  /* F11 */
	case 0x45: return 88;  /* F12 */

	case 0x46: return 99;   /* SYSRQ */
	case 0x47: return 70;   /* SCROLLLOCK */
	case 0x48: return 119;  /* PAUSE */
	case 0x49: return 110;  /* INSERT */
	case 0x4A: return 102;  /* HOME */
	case 0x4B: return 104;  /* PAGEUP */
	case 0x4C: return 111;  /* DELETE */
	case 0x4D: return 107;  /* END */
	case 0x4E: return 109;  /* PAGEDOWN */
	case 0x4F: return 106;  /* RIGHT */
	case 0x50: return 105;  /* LEFT */
	case 0x51: return 108;  /* DOWN */
	case 0x52: return 103;  /* UP */

	case 0x53: return 69;  /* NUMLOCK */
	case 0x54: return 98;  /* KPSLASH */
	case 0x55: return 55;  /* KPASTERISK */
	case 0x56: return 74;  /* KPMINUS */
	case 0x57: return 78;  /* KPPLUS */
	case 0x58: return 96;  /* KPENTER */
	case 0x59: return 79;  /* KP1 */
	case 0x5A: return 80;  /* KP2 */
	case 0x5B: return 81;  /* KP3 */
	case 0x5C: return 75;  /* KP4 */
	case 0x5D: return 76;  /* KP5 */
	case 0x5E: return 77;  /* KP6 */
	case 0x5F: return 71;  /* KP7 */
	case 0x60: return 72;  /* KP8 */
	case 0x61: return 73;  /* KP9 */
	case 0x62: return 82;  /* KP0 */
	case 0x63: return 83;  /* KPDOT */
	case 0x65: return 139; /* MENU */

	case 0xE0: return 29;   /* LEFTCTRL */
	case 0xE1: return 42;   /* LEFTSHIFT */
	case 0xE2: return 56;   /* LEFTALT */
	case 0xE3: return 125;  /* LEFTMETA */
	case 0xE4: return 97;   /* RIGHTCTRL */
	case 0xE5: return 54;   /* RIGHTSHIFT */
	case 0xE6: return 100;  /* RIGHTALT */
	case 0xE7: return 126;  /* RIGHTMETA */
	default:
		return 0;
	}
}

static bool key_found(const uint8_t *src, uint8_t key, unsigned int len)
{
	for (unsigned int i = 0; i < len; i++) {
		if (src[i] == key)
			return true;
	}
	return false;
}

static void send_key_event(bool is_down, uint8_t hid_usage)
{
	int keycode;

	if (!globals.kbd)
		return;

	keycode = hid_usage_to_linux_keycode(hid_usage);
	if (!keycode)
		return;
	ps2_put_keycode(globals.kbd, is_down, keycode);
}

static void hid_keyboard_report_cb(const uint8_t *data, int length)
{
	static uint8_t prev_keys[HID_KEYBOARD_KEY_MAX];
	static uint8_t prev_mod;
	hid_keyboard_input_report_boot_t *kb_report;
	uint8_t mod;

	if (!globals.kbd)
		return;
	if (length < sizeof(hid_keyboard_input_report_boot_t))
		return;

	kb_report = (hid_keyboard_input_report_boot_t *)data;
	mod = kb_report->modifier.val;

	for (int bit = 0; bit < 8; bit++) {
		bool is_down = ((mod >> bit) & 1) != 0;
		bool was_down = ((prev_mod >> bit) & 1) != 0;

		if (is_down != was_down)
			send_key_event(is_down, (uint8_t)(0xE0 + bit));
	}

	for (int i = 0; i < HID_KEYBOARD_KEY_MAX; i++) {
		uint8_t old_key = prev_keys[i];
		uint8_t new_key = kb_report->key[i];

		if (old_key > HID_KEY_ERROR_UNDEFINED &&
		    !key_found(kb_report->key, old_key, HID_KEYBOARD_KEY_MAX)) {
			send_key_event(false, old_key);
		}

		if (new_key > HID_KEY_ERROR_UNDEFINED &&
		    !key_found(prev_keys, new_key, HID_KEYBOARD_KEY_MAX)) {
			send_key_event(true, new_key);
		}
	}

	prev_mod = mod;
	memcpy(prev_keys, kb_report->key, HID_KEYBOARD_KEY_MAX);
}

static void hid_mouse_report_cb(const uint8_t *data, int length)
{
	uint8_t buttons_raw;
	int8_t dx;
	int8_t dy;
	int buttons = 0;

	if (!globals.mouse)
		return;
	if (length < 3)
		return;

	buttons_raw = data[0];
	dx = (int8_t)data[1];
	dy = (int8_t)data[2];

	/* Ignore obviously broken packets from unstable adapters. */
	if (dx > 80 || dx < -80 || dy > 80 || dy < -80)
		return;

	if (buttons_raw & 0x01)
		buttons |= 1;
	if (buttons_raw & 0x02)
		buttons |= 2;
	if (buttons_raw & 0x04)
		buttons |= 4;

	ps2_mouse_event(globals.mouse, dx, dy, 0, buttons);
}

static void hid_iface_cb(hid_host_device_handle_t hid_dev,
			 const hid_host_interface_event_t event, void *arg)
{
	uint8_t data[64];
	size_t data_len = 0;
	hid_host_dev_params_t dev_params;

	(void)arg;
	if (hid_host_device_get_params(hid_dev, &dev_params) != ESP_OK)
		return;

	switch (event) {
	case HID_HOST_INTERFACE_EVENT_INPUT_REPORT:
		if (hid_host_device_get_raw_input_report_data(hid_dev, data, sizeof(data),
							      &data_len) != ESP_OK) {
			return;
		}
		if (dev_params.sub_class != HID_SUBCLASS_BOOT_INTERFACE)
			return;
		if (dev_params.proto == HID_PROTOCOL_KEYBOARD)
			hid_keyboard_report_cb(data, (int)data_len);
		else if (dev_params.proto == HID_PROTOCOL_MOUSE)
			hid_mouse_report_cb(data, (int)data_len);
		break;
	case HID_HOST_INTERFACE_EVENT_DISCONNECTED:
		ESP_LOGI(TAG, "HID device disconnected (proto=%d)", dev_params.proto);
		ESP_ERROR_CHECK(hid_host_device_close(hid_dev));
		break;
	case HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR:
		ESP_LOGW(TAG, "HID transfer error (proto=%d)", dev_params.proto);
		break;
	default:
		break;
	}
}

static void hid_device_event(hid_host_device_handle_t hid_dev,
			     hid_host_driver_event_t event, void *arg)
{
	hid_host_dev_params_t dev_params;
	const hid_host_device_config_t dev_config = {
		.callback = hid_iface_cb,
		.callback_arg = NULL,
	};

	(void)arg;
	if (event != HID_HOST_DRIVER_EVENT_CONNECTED)
		return;
	if (hid_host_device_get_params(hid_dev, &dev_params) != ESP_OK)
		return;

	ESP_LOGI(TAG, "HID device connected (proto=%d subclass=%d)",
		 dev_params.proto, dev_params.sub_class);

	if (dev_params.proto == HID_PROTOCOL_NONE)
		return;

	ESP_ERROR_CHECK(hid_host_device_open(hid_dev, &dev_config));
	if (dev_params.sub_class == HID_SUBCLASS_BOOT_INTERFACE) {
		ESP_ERROR_CHECK(hid_class_request_set_protocol(hid_dev, HID_REPORT_PROTOCOL_BOOT));
		if (dev_params.proto == HID_PROTOCOL_KEYBOARD)
			ESP_ERROR_CHECK(hid_class_request_set_idle(hid_dev, 0, 0));
	}
	ESP_ERROR_CHECK(hid_host_device_start(hid_dev));
}

static void hid_driver_cb(hid_host_device_handle_t hid_dev,
			  const hid_host_driver_event_t event, void *arg)
{
	usb_app_event_t evt = {
		.handle = hid_dev,
		.event = event,
		.arg = arg,
	};

	if (!s_app_event_queue)
		return;
	xQueueSend(s_app_event_queue, &evt, 0);
}

static void usb_lib_task(void *arg)
{
	const usb_host_config_t host_config = {
		.skip_phy_setup = false,
		.intr_flags = ESP_INTR_FLAG_LEVEL1,
	};
	TaskHandle_t notify_task = (TaskHandle_t)arg;

	ESP_ERROR_CHECK(usb_host_install(&host_config));
	xTaskNotifyGive(notify_task);

	while (1) {
		uint32_t event_flags = 0;
		usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
	}
}

static void usb_event_task(void *arg)
{
	usb_app_event_t evt;

	(void)arg;
	while (1) {
		if (xQueueReceive(s_app_event_queue, &evt, portMAX_DELAY))
			hid_device_event(evt.handle, evt.event, evt.arg);
	}
}

void usb_setup(void)
{
	BaseType_t ok;
	const hid_host_driver_config_t hid_cfg = {
		.create_background_task = true,
		.task_priority = 5,
		.stack_size = 4096,
		.core_id = 0,
		.callback = hid_driver_cb,
		.callback_arg = NULL,
	};

	if (s_usb_started)
		return;

	s_app_event_queue = xQueueCreate(16, sizeof(usb_app_event_t));
	if (!s_app_event_queue) {
		ESP_LOGE(TAG, "failed to create usb event queue");
		return;
	}

	ok = xTaskCreatePinnedToCore(usb_lib_task, "usb_events", 4096,
				     xTaskGetCurrentTaskHandle(), 2, NULL, 0);
	if (ok != pdTRUE) {
		ESP_LOGE(TAG, "failed to create usb_lib_task");
		return;
	}
	ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000));

	ESP_ERROR_CHECK(hid_host_install(&hid_cfg));

	ok = xTaskCreatePinnedToCore(usb_event_task, "usb_hid_events", 4096,
				     NULL, 1, NULL, 0);
	if (ok != pdTRUE) {
		ESP_LOGE(TAG, "failed to create usb_event_task");
		return;
	}

	s_usb_started = true;
	ESP_LOGI(TAG, "USB host HID enabled");
}
