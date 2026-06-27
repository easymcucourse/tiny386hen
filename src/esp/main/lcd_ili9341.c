#ifdef USE_LCD_ILI9341

#include <stdio.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_ili9341.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "common.h"
#include "startup_splash.h"

static const char *TAG = "lcd_ili9341";

#define SPLASH_MIN_VISIBLE_MS 2000

void pc_vga_step(void *o);

void lcd_draw(int x_start, int y_start, int x_end, int y_end, void *src)
{
    if (globals.panel) {
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(
            globals.panel,
            x_start, y_start,
            x_end, y_end,
            src));
    }
}

void vga_task(void *arg)
{
    (void)arg;
    int core_id = esp_cpu_get_core_id();
    fprintf(stderr, "vga runs on core %d\n", core_id);

    ESP_LOGI(TAG, "Init ILI9341 panel");

    gpio_config_t blk_cfg = {
        .pin_bit_mask = 1ULL << LCD_SPI_BL,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&blk_cfg));
    gpio_set_level(LCD_SPI_BL, 1);

    spi_bus_config_t bus_cfg = {
        .sclk_io_num = LCD_SPI_SCLK,
        .mosi_io_num = LCD_SPI_MOSI,
        .miso_io_num = LCD_SPI_MISO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * 32 * 2,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_SPI_DC,
        .cs_gpio_num = LCD_SPI_CS,
        .pclk_hz = LCD_SPI_CLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)LCD_SPI_HOST, &io_cfg, &io_handle));

    esp_lcd_panel_handle_t panel = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_SPI_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
        .bits_per_pixel = BPP,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_cfg, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    globals.panel = panel;

    int64_t splash_start_us = esp_timer_get_time();
    bool logo_ok = startup_splash_draw_logo();
    ESP_LOGI(TAG, "startup logo %s", logo_ok ? "shown" : "not shown");
    xEventGroupSetBits(global_event_group, TINY386_EVENT_LOGO_READY);
    int64_t elapsed_ms = (esp_timer_get_time() - splash_start_us) / 1000;
    if (elapsed_ms < SPLASH_MIN_VISIBLE_MS) {
        vTaskDelay(pdMS_TO_TICKS(SPLASH_MIN_VISIBLE_MS - elapsed_ms));
    }
    xEventGroupSetBits(global_event_group, TINY386_EVENT_SPLASH_DONE);
    ESP_LOGI(TAG, "waiting for Booting from 0000:7c00 before VM display");
    xEventGroupWaitBits(global_event_group,
                        TINY386_EVENT_BOOT_SECTOR,
                        pdFALSE,
                        pdFALSE,
                        portMAX_DELAY);
    ESP_LOGI(TAG, "boot sector reached, switching to VM display");

    while (1) {
        pc_vga_step(globals.pc);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

#endif
