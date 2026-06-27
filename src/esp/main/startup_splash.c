#include "startup_splash.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2s_std.h"
#include "esp_partition.h"

#include "common.h"

#define RES_MAGIC "T386RES"
#define RES_VERSION 1
#define LOGO_ID "LOGO565"
#define MUSIC_ID "MUSICPCM"
#define LCD_STRIP_ROWS 4

void lcd_draw(int x_start, int y_start, int x_end, int y_end, void *src);
void lcd_wait_flush_done(void);

typedef struct {
	char magic[8];
	uint16_t version;
	uint16_t count;
	uint32_t reserved;
} __attribute__((packed)) resource_header_t;

typedef struct {
	char id[8];
	uint32_t offset;
	uint32_t size;
	uint16_t meta0;
	uint16_t meta1;
	uint16_t meta2;
	uint16_t meta3;
	uint8_t reserved[8];
} __attribute__((packed)) resource_entry_t;

static const esp_partition_t *s_res_part;
static resource_header_t s_res_header;
static resource_entry_t s_res_entries[4];

static uint16_t read_u16(const void *p)
{
	uint16_t v;
	memcpy(&v, p, sizeof(v));
	return v;
}

static uint32_t read_u32(const void *p)
{
	uint32_t v;
	memcpy(&v, p, sizeof(v));
	return v;
}

static bool resource_map(void)
{
	if (s_res_part)
		return true;

	const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
							       ESP_PARTITION_SUBTYPE_ANY,
							       "resources");
	if (!part) {
		fprintf(stderr, "[splash] resources partition not found\n");
		return false;
	}

	esp_err_t ret = esp_partition_read(part, 0, &s_res_header, sizeof(s_res_header));
	if (ret != ESP_OK) {
		fprintf(stderr, "[splash] resources header read failed: %d\n", (int)ret);
		return false;
	}

	if (memcmp(s_res_header.magic, RES_MAGIC, 7) != 0 ||
	    read_u16(&s_res_header.version) != RES_VERSION) {
		fprintf(stderr,
			"[splash] invalid resources header: %02x %02x %02x %02x %02x %02x %02x %02x ver=%u\n",
			s_res_header.magic[0], s_res_header.magic[1],
			s_res_header.magic[2], s_res_header.magic[3],
			s_res_header.magic[4], s_res_header.magic[5],
			s_res_header.magic[6], s_res_header.magic[7],
			read_u16(&s_res_header.version));
		return false;
	}

	uint16_t count = read_u16(&s_res_header.count);
	if (count > sizeof(s_res_entries) / sizeof(s_res_entries[0])) {
		fprintf(stderr, "[splash] too many resource entries: %u\n", count);
		return false;
	}

	ret = esp_partition_read(part, sizeof(s_res_header),
				 s_res_entries, count * sizeof(s_res_entries[0]));
	if (ret != ESP_OK) {
		fprintf(stderr, "[splash] resources entries read failed: %d\n", (int)ret);
		return false;
	}

	s_res_part = part;
	fprintf(stderr, "[splash] resources ready: offset=0x%lx size=%lu count=%u\n",
		(unsigned long)part->address, (unsigned long)part->size, count);
	return true;
}

static const resource_entry_t *find_entry(const char *id)
{
	if (!resource_map())
		return NULL;

	uint16_t count = read_u16(&s_res_header.count);
	for (uint16_t i = 0; i < count; i++) {
		uint32_t off = read_u32(&s_res_entries[i].offset);
		uint32_t size = read_u32(&s_res_entries[i].size);
		if (off > s_res_part->size || size > s_res_part->size - off)
			continue;
		if (memcmp(s_res_entries[i].id, id, 7) == 0)
			return &s_res_entries[i];
	}
	fprintf(stderr, "[splash] resource not found: %s\n", id);
	return NULL;
}

void startup_resources_mount(void)
{
	resource_map();
}

bool startup_splash_draw_logo(void)
{
	const resource_entry_t *entry = find_entry(LOGO_ID);
	if (!entry)
		return false;
	uint16_t *lcd_strip = malloc(LCD_HEIGHT * LCD_STRIP_ROWS * sizeof(*lcd_strip));
	if (!lcd_strip) {
		fprintf(stderr, "[splash] logo buffer allocation failed\n");
		return false;
	}

	uint32_t offset = read_u32(&entry->offset);
	uint32_t size = read_u32(&entry->size);
	uint16_t width = read_u16(&entry->meta0);
	uint16_t height = read_u16(&entry->meta1);
	if (width != LCD_HEIGHT || height != LCD_WIDTH ||
	    size < (uint32_t)width * height * sizeof(uint16_t)) {
		fprintf(stderr,
			"[splash] invalid logo: %ux%u size=%lu expected=%lu\n",
			width, height, (unsigned long)size,
			(unsigned long)LCD_HEIGHT * LCD_WIDTH * sizeof(uint16_t));
		free(lcd_strip);
		return false;
	}

	fprintf(stderr, "[splash] drawing logo: %ux%u offset=0x%lx size=%lu\n",
		width, height, (unsigned long)offset, (unsigned long)size);
	for (int y = 0; y < LCD_WIDTH; y += LCD_STRIP_ROWS) {
		int rows = LCD_STRIP_ROWS;
		if (rows > LCD_WIDTH - y)
			rows = LCD_WIDTH - y;
		size_t bytes = (size_t)rows * LCD_HEIGHT * sizeof(uint16_t);
		esp_err_t ret = esp_partition_read(s_res_part,
						   offset + (size_t)y * LCD_HEIGHT * sizeof(uint16_t),
						   lcd_strip, bytes);
		if (ret != ESP_OK) {
			fprintf(stderr, "[splash] logo read failed y=%d ret=%d\n", y, (int)ret);
			free(lcd_strip);
			return false;
		}
		lcd_draw(0, y, LCD_HEIGHT, y + rows, lcd_strip);
		lcd_wait_flush_done();
	}
	free(lcd_strip);
	fprintf(stderr, "[splash] logo draw complete\n");
	return true;
}

bool startup_splash_play_wav(void *i2s_tx_chan)
{
	i2s_chan_handle_t tx = (i2s_chan_handle_t)i2s_tx_chan;
	const resource_entry_t *entry = find_entry(MUSIC_ID);
	if (!entry)
		return false;
	uint8_t *i2s_dma_buf = malloc(1024);
	if (!i2s_dma_buf) {
		fprintf(stderr, "[splash] music buffer allocation failed\n");
		return false;
	}

	uint32_t offset = read_u32(&entry->offset);
	uint32_t size = read_u32(&entry->size);
	uint16_t rate = read_u16(&entry->meta0);
	uint16_t channels = read_u16(&entry->meta1);
	uint16_t bits = read_u16(&entry->meta2);
	if (rate != 44100 || channels != 2 || bits != 16 ||
	    (size & 3) != 0) {
		fprintf(stderr,
			"[splash] invalid music: rate=%u channels=%u bits=%u size=%lu\n",
			rate, channels, bits, (unsigned long)size);
		free(i2s_dma_buf);
		return false;
	}

	fprintf(stderr, "[splash] playing music: rate=%u channels=%u bits=%u size=%lu\n",
		rate, channels, bits, (unsigned long)size);
	uint32_t pos = offset;
	while (size > 0) {
		size_t chunk = size > 1024 ? 1024 : size;
		size_t bwritten;
		esp_err_t ret = esp_partition_read(s_res_part, pos, i2s_dma_buf, chunk);
		if (ret != ESP_OK) {
			fprintf(stderr, "[splash] music read failed pos=0x%lx ret=%d\n",
				(unsigned long)pos, (int)ret);
			free(i2s_dma_buf);
			return false;
		}
		if (i2s_channel_write(tx, i2s_dma_buf, chunk,
				      &bwritten, portMAX_DELAY) != ESP_OK) {
			free(i2s_dma_buf);
			return false;
		}
		if (bwritten != chunk) {
			fprintf(stderr, "[splash] music short write: %u/%u\n",
				(unsigned)bwritten, (unsigned)chunk);
			free(i2s_dma_buf);
			return false;
		}
		pos += chunk;
		size -= chunk;
	}
	memset(i2s_dma_buf, 0, 1024);
	for (int i = 0; i < 4; i++) {
		size_t bwritten;
		i2s_channel_write(tx, i2s_dma_buf, 1024,
				  &bwritten, portMAX_DELAY);
	}
	free(i2s_dma_buf);
	fprintf(stderr, "[splash] music play complete\n");
	return true;
}
