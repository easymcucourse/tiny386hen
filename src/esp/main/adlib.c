/*
 * Adlib OPL2 backend for tiny386hen using emu8950.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "adlib.h"
#include "emu8950.h"

#define ADLIB_OPL_CLOCK 3579545
#define ADLIB_SAMPLE_RATE 44100

struct AdlibState {
	uint32_t port;
	int active;
	int enabled;
	OPL *opl;
};

void adlib_write(void *opaque, uint32_t nport, uint32_t val)
{
	AdlibState *s = opaque;

	if (!s || !s->opl) {
		return;
	}

	s->active = 1;
	s->port = nport & 3;
	OPL_writeIO(s->opl, s->port, (uint8_t)val);
}

uint32_t adlib_read(void *opaque, uint32_t nport)
{
	AdlibState *s = opaque;

	if (!s || !s->opl) {
		return (uint32_t)-1;
	}

	if ((nport & 3) == 0) {
		return OPL_status(s->opl);
	}
	return 0;
}

void adlib_callback(void *opaque, uint8_t *stream, int free)
{
	AdlibState *s = opaque;
	int16_t *out = (int16_t *)stream;
	int samples = free / (int)sizeof(int16_t);

	if (!s || !s->opl || !s->active || !s->enabled || samples <= 0) {
		return;
	}

	for (int i = 0; i < samples; i++) {
		out[i] = OPL_calc(s->opl);
	}
}

void adlib_free(AdlibState *s)
{
	if (!s) {
		return;
	}

	if (s->opl) {
		OPL_delete(s->opl);
		s->opl = NULL;
	}
	free(s);
}

AdlibState *adlib_new(void)
{
	AdlibState *s = calloc(1, sizeof(*s));

	if (!s) {
		return NULL;
	}

	s->opl = OPL_new(ADLIB_OPL_CLOCK, ADLIB_SAMPLE_RATE);
	if (!s->opl) {
		free(s);
		return NULL;
	}

	OPL_setChipType(s->opl, 2); /* YM3812 / OPL2 */
	s->enabled = 1;
	return s;
}
