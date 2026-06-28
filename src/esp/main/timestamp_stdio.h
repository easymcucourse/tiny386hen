#pragma once

#ifdef BUILD_ESP32
#include <stdarg.h>
#include <stdio.h>

int tiny386_ts_fprintf(FILE *stream, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));
int tiny386_ts_printf(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));
int tiny386_ts_putchar(int c);
int tiny386_ts_puts(const char *s);

#ifndef TINY386_TIMESTAMP_STDIO_IMPL
#define fprintf tiny386_ts_fprintf
#define printf  tiny386_ts_printf
#define putchar tiny386_ts_putchar
#define puts    tiny386_ts_puts
#endif

#endif /* BUILD_ESP32 */
