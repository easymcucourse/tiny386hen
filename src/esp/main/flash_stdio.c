#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include "esp_partition.h"

#define FLASH_FILE_MAGIC 0x46464c53u
#define FLASH_FILE_MAX 2
#define FLASH_SECTOR_SIZE 4096

typedef struct {
	uint32_t magic;
	bool in_use;
	bool writable;
	off_t pos;
	const esp_partition_t *part;
	uint8_t sector[FLASH_SECTOR_SIZE];
} flash_file_t;

static flash_file_t flash_files[FLASH_FILE_MAX];

extern FILE *__real_fopen(const char *path, const char *mode);
extern size_t __real_fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
extern size_t __real_fwrite(const void *ptr, size_t size, size_t nmemb,
			    FILE *stream);
extern int __real_fseeko(FILE *stream, off_t offset, int whence);
extern off_t __real_ftello(FILE *stream);
extern int __real_fclose(FILE *stream);

static flash_file_t *flash_file_from_stream(FILE *stream)
{
	flash_file_t *ff = (flash_file_t *)stream;

	if (!stream)
		return NULL;
	for (int i = 0; i < FLASH_FILE_MAX; i++) {
		if (&flash_files[i] == ff && ff->in_use &&
		    ff->magic == FLASH_FILE_MAGIC)
			return ff;
	}
	return NULL;
}

FILE *__wrap_fopen(const char *path, const char *mode)
{
	const esp_partition_t *part;
	flash_file_t *ff = NULL;
	const char *label;

	if (!path || strncmp(path, "flash:", 6) != 0)
		return __real_fopen(path, mode);

	if (!mode || strchr(mode, 'w') || strchr(mode, 'a')) {
		errno = EINVAL;
		return NULL;
	}

	label = path + 6;
	part = esp_partition_find_first(ESP_PARTITION_TYPE_ANY,
					ESP_PARTITION_SUBTYPE_ANY, label);
	if (!part) {
		errno = ENOENT;
		return NULL;
	}

	for (int i = 0; i < FLASH_FILE_MAX; i++) {
		if (!flash_files[i].in_use) {
			ff = &flash_files[i];
			break;
		}
	}
	if (!ff) {
		errno = EMFILE;
		return NULL;
	}

	memset(ff, 0, sizeof(*ff));
	ff->magic = FLASH_FILE_MAGIC;
	ff->in_use = true;
	ff->writable = mode && strchr(mode, '+');
	ff->part = part;
	return (FILE *)ff;
}

size_t __wrap_fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	flash_file_t *ff = flash_file_from_stream(stream);
	size_t bytes;

	if (!ff)
		return __real_fread(ptr, size, nmemb, stream);
	if (!size || !nmemb)
		return 0;

	bytes = size * nmemb;
	if (ff->pos >= (off_t)ff->part->size)
		return 0;
	if ((off_t)bytes > (off_t)ff->part->size - ff->pos)
		bytes = ff->part->size - ff->pos;
	if (esp_partition_read(ff->part, ff->pos, ptr, bytes) != ESP_OK)
		return 0;

	ff->pos += bytes;
	return bytes / size;
}

size_t __wrap_fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
	flash_file_t *ff = flash_file_from_stream(stream);
	const uint8_t *src = (const uint8_t *)ptr;
	size_t bytes;
	size_t done = 0;

	if (!ff)
		return __real_fwrite(ptr, size, nmemb, stream);
	if (!size || !nmemb)
		return 0;
	if (!ff->writable) {
		errno = EBADF;
		return 0;
	}

	bytes = size * nmemb;
	if (ff->pos < 0 || (off_t)bytes > (off_t)ff->part->size - ff->pos) {
		errno = ENOSPC;
		return 0;
	}

	while (done < bytes) {
		size_t sector_off = (ff->pos + done) % FLASH_SECTOR_SIZE;
		size_t sector_base = (ff->pos + done) - sector_off;
		size_t chunk = FLASH_SECTOR_SIZE - sector_off;

		if (chunk > bytes - done)
			chunk = bytes - done;
		if (esp_partition_read(ff->part, sector_base, ff->sector,
				       FLASH_SECTOR_SIZE) != ESP_OK)
			break;
		memcpy(ff->sector + sector_off, src + done, chunk);
		if (esp_partition_erase_range(ff->part, sector_base,
					      FLASH_SECTOR_SIZE) != ESP_OK)
			break;
		if (esp_partition_write(ff->part, sector_base, ff->sector,
					FLASH_SECTOR_SIZE) != ESP_OK)
			break;
		done += chunk;
	}

	ff->pos += done;
	return done / size;
}

int __wrap_fseeko(FILE *stream, off_t offset, int whence)
{
	flash_file_t *ff = flash_file_from_stream(stream);
	off_t pos;

	if (!ff)
		return __real_fseeko(stream, offset, whence);

	switch (whence) {
	case SEEK_SET:
		pos = offset;
		break;
	case SEEK_CUR:
		pos = ff->pos + offset;
		break;
	case SEEK_END:
		pos = (off_t)ff->part->size + offset;
		break;
	default:
		errno = EINVAL;
		return -1;
	}

	if (pos < 0 || pos > (off_t)ff->part->size) {
		errno = EINVAL;
		return -1;
	}
	ff->pos = pos;
	return 0;
}

off_t __wrap_ftello(FILE *stream)
{
	flash_file_t *ff = flash_file_from_stream(stream);

	if (!ff)
		return __real_ftello(stream);
	return ff->pos;
}

int __wrap_fclose(FILE *stream)
{
	flash_file_t *ff = flash_file_from_stream(stream);

	if (!ff)
		return __real_fclose(stream);
	memset(ff, 0, sizeof(*ff));
	return 0;
}
