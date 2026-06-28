#define TINY386_TIMESTAMP_STDIO_IMPL
#include "timestamp_stdio.h"

#ifdef BUILD_ESP32
#include <stdbool.h>
#include <stdint.h>
#include "esp_timer.h"

static bool stdout_line_start = true;
static bool stderr_line_start = true;

static bool *line_state(FILE *stream)
{
	return stream == stderr ? &stderr_line_start : &stdout_line_start;
}

static void write_prefix(FILE *stream)
{
	fprintf(stream, "[%llu ms] ",
		(unsigned long long)(esp_timer_get_time() / 1000ULL));
}

static int write_char(FILE *stream, int c)
{
	bool *at_line_start = line_state(stream);

	if (*at_line_start && c != '\r' && c != '\n') {
		write_prefix(stream);
		*at_line_start = false;
	}

	fputc(c, stream);
	if (c == '\n')
		*at_line_start = true;
	return c;
}

static int write_buffer(FILE *stream, const char *buf, int len)
{
	for (int i = 0; i < len; i++)
		write_char(stream, (unsigned char)buf[i]);
	fflush(stream);
	return len;
}

int tiny386_ts_fprintf(FILE *stream, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (len < 0)
		return len;
	if (len >= (int)sizeof(buf))
		len = (int)sizeof(buf) - 1;
	return write_buffer(stream, buf, len);
}

int tiny386_ts_printf(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	int len;

	va_start(ap, fmt);
	len = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (len < 0)
		return len;
	if (len >= (int)sizeof(buf))
		len = (int)sizeof(buf) - 1;
	return write_buffer(stdout, buf, len);
}

int tiny386_ts_putchar(int c)
{
	write_char(stdout, c);
	fflush(stdout);
	return c;
}

int tiny386_ts_puts(const char *s)
{
	int len = 0;

	while (s[len] != '\0') {
		write_char(stdout, (unsigned char)s[len]);
		len++;
	}
	write_char(stdout, '\n');
	fflush(stdout);
	return len + 1;
}
#endif /* BUILD_ESP32 */
