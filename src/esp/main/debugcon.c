#include "debugcon.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_cpu.h"

#ifndef DEBUGCON_BUF_SIZE
#define DEBUGCON_BUF_SIZE (16 * 1024)
#endif

#ifndef DEBUGCON_DRAIN_CHUNK
#define DEBUGCON_DRAIN_CHUNK 256
#endif

#ifndef DEBUGCON_TASK_STACK
#define DEBUGCON_TASK_STACK 4096
#endif

void *psmalloc(long size);

static uint8_t *s_debugcon_buf;
static uint32_t s_debugcon_capacity;
static atomic_uint s_debugcon_head;
static atomic_uint s_debugcon_tail;
static atomic_uint s_debugcon_dropped;

static void debugcon_task(void *arg)
{
	uint8_t local[DEBUGCON_DRAIN_CHUNK];

	(void)arg;
	fprintf(stderr, "debugcon runs on core %d\n", esp_cpu_get_core_id());

	for (;;) {
		uint32_t tail = atomic_load_explicit(&s_debugcon_tail,
						     memory_order_relaxed);
		uint32_t head = atomic_load_explicit(&s_debugcon_head,
						     memory_order_acquire);
		uint32_t avail = head - tail;

		if (!avail) {
			vTaskDelay(1);
			continue;
		}

		uint32_t n = avail;
		if (n > sizeof(local))
			n = sizeof(local);
		for (uint32_t i = 0; i < n; i++)
			local[i] = s_debugcon_buf[(tail + i) % s_debugcon_capacity];

		uint32_t new_tail = tail + n;
		uint32_t cur_tail = atomic_load_explicit(&s_debugcon_tail,
							 memory_order_acquire);
		if ((int32_t)(new_tail - cur_tail) > 0) {
			atomic_store_explicit(&s_debugcon_tail, new_tail,
					      memory_order_release);
		}
		fwrite(local, 1, n, stdout);
		fflush(stdout);
	}
}

void debugcon_init(void)
{
	if (s_debugcon_buf)
		return;

	s_debugcon_capacity = DEBUGCON_BUF_SIZE;
	s_debugcon_buf = psmalloc(s_debugcon_capacity);
	memset(s_debugcon_buf, 0, s_debugcon_capacity);
	atomic_store_explicit(&s_debugcon_head, 0, memory_order_relaxed);
	atomic_store_explicit(&s_debugcon_tail, 0, memory_order_relaxed);
	atomic_store_explicit(&s_debugcon_dropped, 0, memory_order_relaxed);

	xTaskCreatePinnedToCore(debugcon_task, "debugcon", DEBUGCON_TASK_STACK,
				NULL, 0, NULL, 0);
}

void debugcon_write_char(uint8_t ch)
{
	uint32_t head;
	uint32_t tail;

	if (!s_debugcon_buf)
		return;

	head = atomic_load_explicit(&s_debugcon_head, memory_order_relaxed);
	tail = atomic_load_explicit(&s_debugcon_tail, memory_order_acquire);
	if (head - tail >= s_debugcon_capacity) {
		atomic_store_explicit(&s_debugcon_tail, tail + 1,
				      memory_order_release);
		atomic_fetch_add_explicit(&s_debugcon_dropped, 1,
					  memory_order_relaxed);
	}

	s_debugcon_buf[head % s_debugcon_capacity] = ch;
	atomic_store_explicit(&s_debugcon_head, head + 1, memory_order_release);
}
