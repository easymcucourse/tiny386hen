#define BUILD_ESP32

/* No WiFi on this board (offline LCD + SD only) */
#define TINY386_NO_WIFI 1

#define IRAM_ATTR_CPU_EXEC1 _SECTION_ATTR_IMPL(".tiny386_cpu_iram", __COUNTER__)

#define BPP 16
#define FULL_UPDATE
#define SWAPXY

/* tiny386 logical frame (landscape) */
#define LCD_WIDTH  320
#define LCD_HEIGHT 240

/* ILI9341 panel (portrait 240x320, SPI) */
#define USE_LCD_ILI9341
#define LCD_SPI_HOST       SPI2_HOST
#define LCD_SPI_CS         4
#define LCD_SPI_RST        5
#define LCD_SPI_DC         6
#define LCD_SPI_MOSI       7
#define LCD_SPI_SCLK       15
#define LCD_SPI_BL         16
#define LCD_SPI_MISO       17

/* I2S audio output */
#define I2S_MCLK I2S_GPIO_UNUSED
#define I2S_BCLK 40
#define I2S_WS   42
#define I2S_DOUT 41
#define MIXER_BUF_LEN 512



/* This panel/wiring has been verified with the reference firmware at 80MHz.
 * Slower clocks can leave some modules blank with the ESP LCD SPI driver.
 */
#define LCD_SPI_CLK_HZ     (40000000)
#define LCD_ILI9341_H_RES  240
#define LCD_ILI9341_V_RES  320

/* SD card disabled: GPIO18/19 conflict with USB OTG D+/D- */

/*
 * Allocate the psmalloc arena from the SPIRAM heap rather than using the raw
 * PSRAM pointer directly.  Without this, psmalloc and the ESP-IDF SPIRAM heap
 * overlap (both start at psram+0), causing psmalloc to overwrite FATFS/VFS
 * structures that storage_init placed in the heap and corrupting function
 * pointers (=> InstrFetchProhibited at PC=0 during BIOS execution).
 *
 * Value: 4MB phys_mem + 320KB vga_mem + 256KB framebuffer + 256KB headroom.
 */
#define PSRAM_ALLOC_LEN (5 * 1024 * 1024)

/*
 * 2:1 downscale rendering so that VGA 80-column text mode (640×400 pixels
 * with force_8dm) fits in the 320×240 logical frame.
 * Without this, vga_text_refresh() does: 320 < 640 → early return (blank).
 */
//#define SCALE_2_1
#define SCALE_CROP
