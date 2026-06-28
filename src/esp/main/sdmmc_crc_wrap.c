/*
 * sdmmc_crc_wrap.c — CMD59 (CRC on/off) workaround for SPI SD cards
 *
 * Some SD/TF cards reject CMD59 with ESP_ERR_NOT_SUPPORTED, which causes
 * the ESP-IDF SPI SD driver to abort the whole init sequence.
 *
 * This link-time wrap intercepts sdmmc_init_spi_crc:
 *   - Calls the real function first.
 *   - If it returns ESP_ERR_NOT_SUPPORTED, issues a best-effort
 *     CMD59(off) directly via the host transaction callback, then
 *     returns ESP_OK so that init continues without CRC.
 *
 * Enabled by linker flags in main/CMakeLists.txt:
 *   -Wl,--wrap=sdmmc_init_spi_crc
 *   -Wl,--undefined=__wrap_sdmmc_init_spi_crc
 *
 * This matches Arduino SD library behaviour (data CRC disabled).
 */

#ifdef BUILD_ESP32

#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"

static const char *TAG = "sdmmc_crc_wrap";

/* Declaration of the real (unwrapped) function provided by the linker */
esp_err_t __real_sdmmc_init_spi_crc(sdmmc_card_t *card);

esp_err_t __wrap_sdmmc_init_spi_crc(sdmmc_card_t *card)
{
	esp_err_t ret = __real_sdmmc_init_spi_crc(card);
	if (ret == ESP_ERR_NOT_SUPPORTED) {
		ESP_LOGW(TAG, "CMD59 not supported, disabling CRC and continuing");
		/*
		 * Best-effort CMD59(off) via the host's do_transaction callback.
		 * If this also fails we still return ESP_OK so that the SD init
		 * can proceed (matching Arduino SD library behaviour).
		 */
		sdmmc_command_t cmd = {
			.opcode   = 59,   /* CMD59: CRC_ON_OFF */
			.arg      = 0,    /* 0 = CRC off */
			.flags    = SCF_CMD_AC | SCF_RSP_R1,
		};
		card->host.do_transaction(card->host.slot, &cmd);
		return ESP_OK;
	}
	return ret;
}

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define SD_CACHE_SECTOR_SIZE 512
#define SD_CACHE_WAYS 4
#define SD_CACHE_SETS 256  // 256 * 4 = 1024 sectors = 512 KB

typedef struct {
	uint32_t sector;
	uint32_t age;
	bool valid;
} sd_cache_entry_t;

static uint8_t *s_cache_data = NULL;
static sd_cache_entry_t s_cache_entries[SD_CACHE_SETS][SD_CACHE_WAYS];
static uint32_t s_cache_age_counter = 0;
static SemaphoreHandle_t s_cache_mutex = NULL;
static bool s_cache_initialized = false;

static void sd_cache_init(void)
{
	if (s_cache_initialized) {
		return;
	}

	// Create mutex
	s_cache_mutex = xSemaphoreCreateMutex();
	if (!s_cache_mutex) {
		ESP_LOGE(TAG, "Failed to create cache mutex");
		return;
	}

	// Allocate contiguous cache buffer in PSRAM with 64-byte alignment
	size_t cache_size = SD_CACHE_SETS * SD_CACHE_WAYS * SD_CACHE_SECTOR_SIZE;
	s_cache_data = heap_caps_aligned_alloc(64, cache_size, MALLOC_CAP_SPIRAM);
	if (!s_cache_data) {
		ESP_LOGE(TAG, "Failed to allocate SD cache buffer in PSRAM");
		vSemaphoreDelete(s_cache_mutex);
		s_cache_mutex = NULL;
		return;
	}

	memset(s_cache_entries, 0, sizeof(s_cache_entries));
	s_cache_initialized = true;
	ESP_LOGI(TAG, "SD Card Sector Cache (512KB, 4-Way, PSRAM aligned) initialized successfully");
}

static bool sd_cache_lookup(uint32_t sector, uint32_t *out_way)
{
	uint32_t set = sector % SD_CACHE_SETS;
	for (int w = 0; w < SD_CACHE_WAYS; w++) {
		if (s_cache_entries[set][w].valid && s_cache_entries[set][w].sector == sector) {
			*out_way = w;
			return true;
		}
	}
	return false;
}

static void sd_cache_insert(uint32_t sector, const uint8_t *data)
{
	uint32_t set = sector % SD_CACHE_SETS;
	uint32_t lru_way = 0;
	uint32_t min_age = 0xFFFFFFFF;

	for (int w = 0; w < SD_CACHE_WAYS; w++) {
		if (!s_cache_entries[set][w].valid) {
			lru_way = w;
			break;
		}
		if (s_cache_entries[set][w].age < min_age) {
			min_age = s_cache_entries[set][w].age;
			lru_way = w;
		}
	}

	s_cache_entries[set][lru_way].sector = sector;
	s_cache_entries[set][lru_way].valid = true;
	s_cache_entries[set][lru_way].age = ++s_cache_age_counter;

	uint32_t buffer_idx = (set * SD_CACHE_WAYS + lru_way) * SD_CACHE_SECTOR_SIZE;
	memcpy(s_cache_data + buffer_idx, data, SD_CACHE_SECTOR_SIZE);
}

static void sd_cache_update(uint32_t sector, const uint8_t *data)
{
	uint32_t set = sector % SD_CACHE_SETS;
	for (int w = 0; w < SD_CACHE_WAYS; w++) {
		if (s_cache_entries[set][w].valid && s_cache_entries[set][w].sector == sector) {
			s_cache_entries[set][w].age = ++s_cache_age_counter;
			uint32_t buffer_idx = (set * SD_CACHE_WAYS + w) * SD_CACHE_SECTOR_SIZE;
			memcpy(s_cache_data + buffer_idx, data, SD_CACHE_SECTOR_SIZE);
			return;
		}
	}
	// If not in cache, insert it so that immediate reads hit
	sd_cache_insert(sector, data);
}

esp_err_t __real_sdmmc_read_sectors(sdmmc_card_t* card, void* dst, size_t start_sector, size_t sector_count);
esp_err_t __real_sdmmc_write_sectors(sdmmc_card_t* card, const void* src, size_t start_sector, size_t sector_count);

esp_err_t __wrap_sdmmc_read_sectors(sdmmc_card_t* card, void* dst, size_t start_sector, size_t sector_count)
{
	// Lazy initialization
	if (!s_cache_initialized) {
		sd_cache_init();
	}

	// If cache initialization failed, bypass cache
	if (!s_cache_initialized) {
		return __real_sdmmc_read_sectors(card, dst, start_sector, sector_count);
	}

	uint8_t *dst_u8 = (uint8_t *)dst;
	size_t i = 0;
	esp_err_t ret = ESP_OK;

	xSemaphoreTake(s_cache_mutex, portMAX_DELAY);

	while (i < sector_count) {
		uint32_t sector = start_sector + i;
		uint32_t way;
		if (sd_cache_lookup(sector, &way)) {
			// Cache Hit: Copy data from PSRAM cache
			uint32_t set = sector % SD_CACHE_SETS;
			uint32_t buffer_idx = (set * SD_CACHE_WAYS + way) * SD_CACHE_SECTOR_SIZE;
			memcpy(dst_u8 + i * SD_CACHE_SECTOR_SIZE, s_cache_data + buffer_idx, SD_CACHE_SECTOR_SIZE);
			i++;
		} else {
			// Cache Miss: find contiguous miss run to minimize SDSPI commands
			size_t miss_start = i;
			size_t miss_count = 0;
			while (i < sector_count) {
				uint32_t next_sec = start_sector + i;
				uint32_t dummy_way;
				if (sd_cache_lookup(next_sec, &dummy_way)) {
					break;
				}
				miss_count++;
				i++;
			}

			// Release mutex during actual SPI read operation to allow other tasks to read/write cache
			xSemaphoreGive(s_cache_mutex);

			ret = __real_sdmmc_read_sectors(card, dst_u8 + miss_start * SD_CACHE_SECTOR_SIZE, start_sector + miss_start, miss_count);
			if (ret != ESP_OK) {
				return ret;
			}

			// Re-acquire mutex to update cache entries
			xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
			for (size_t m = 0; m < miss_count; m++) {
				sd_cache_insert(start_sector + miss_start + m, dst_u8 + (miss_start + m) * SD_CACHE_SECTOR_SIZE);
			}
		}
	}

	xSemaphoreGive(s_cache_mutex);
	return ret;
}

esp_err_t __wrap_sdmmc_write_sectors(sdmmc_card_t* card, const void* src, size_t start_sector, size_t sector_count)
{
	if (!s_cache_initialized) {
		sd_cache_init();
	}

	// Write-through: write to SD card first
	esp_err_t ret = __real_sdmmc_write_sectors(card, src, start_sector, sector_count);
	if (ret != ESP_OK) {
		return ret;
	}

	if (s_cache_initialized) {
		xSemaphoreTake(s_cache_mutex, portMAX_DELAY);
		const uint8_t *src_u8 = (const uint8_t *)src;
		for (size_t i = 0; i < sector_count; i++) {
			sd_cache_update(start_sector + i, src_u8 + i * SD_CACHE_SECTOR_SIZE);
		}
		xSemaphoreGive(s_cache_mutex);
	}

	return ESP_OK;
}

#endif /* BUILD_ESP32 */
