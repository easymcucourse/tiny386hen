#ifndef COMMON_H
#define COMMON_H

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

struct Globals {
	void *pc;
	void *kbd;
	void *mouse;
	void *panel;
	void *panel_fb;   /* RGB panel DMA frame buffer (NULL if not used) */
};

extern EventGroupHandle_t global_event_group;
extern struct Globals globals;
extern int i2s_output_volume_percent;

void i2s_set_output_volume_percent(int volume);

#define TINY386_EVENT_PC_READY     BIT0
#define TINY386_EVENT_SPLASH_DONE  BIT1
#define TINY386_EVENT_AUDIO_DONE   BIT2
#define TINY386_EVENT_LOGO_READY   BIT3
#define TINY386_EVENT_BOOT_SECTOR  BIT4

#endif /* COMMON_H */
