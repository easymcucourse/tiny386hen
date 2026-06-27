#ifndef STARTUP_SPLASH_H
#define STARTUP_SPLASH_H

#include <stdbool.h>

void startup_resources_mount(void);
bool startup_splash_draw_logo(void);
bool startup_splash_play_wav(void *i2s_tx_chan);

#endif /* STARTUP_SPLASH_H */
