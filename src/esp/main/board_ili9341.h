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

/* Run more x86 instructions per device poll to reduce interpreter overhead. */
#define PC_STEP_COUNT 16384
#define PC_PERF_LOG_ENABLED 1
#define PC_PERF_LOG_INTERVAL_US 5000000u
#define PC_CMOS_STEP_DIV 16
#define PC_KBD_STEP_DIV 2
#define PC_DMA_STEP_DIV 2
#define PC_SERIAL_STEP_DIV 2

/* Network is disabled on this board; omit NE2000 state and runtime work. */
#define TINY386_NO_NE2000 1



/* This panel/wiring has been verified with the reference firmware at 80MHz.
 * Slower clocks can leave some modules blank with the ESP LCD SPI driver.
 */
#define LCD_SPI_CLK_HZ     (40000000)
#define LCD_ILI9341_H_RES  240
#define LCD_ILI9341_V_RES  320

/* SD card (SDSPI) */
#define SD_SPI_HOST      SPI3_HOST
#define SD_SPI_CS        9
#define SD_SPI_MOSI      10
#define SD_SPI_MISO      11
#define SD_SPI_SCK       12
#define SD_SPI_FREQ_KHZ  20000

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
 * Set to 1 for board JIT differential self-test only: skips LCD, I2S, SD, USB,
 * and the PC emulator. Serial output uses esp_rom_printf (UART0).
 */
#define TINY386_JIT_SELFTEST_ONLY 0

/*
 * Run jit_selftest at i386_task entry (production boot path, no LCD/I2S/USB/SD).
 */
#define TINY386_JIT_SELFTEST_AT_BOOT 0

/*
 * 2:1 downscale rendering so that VGA 80-column text mode (640×400 pixels
 * with force_8dm) fits in the 320×240 logical frame.
 * Without this, vga_text_refresh() does: 320 < 640 → early return (blank).
 */
//#define SCALE_2_1
#define SCALE_CROP
