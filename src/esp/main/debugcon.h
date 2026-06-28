#ifndef DEBUGCON_H
#define DEBUGCON_H

#include <stdint.h>

void debugcon_init(void);
void debugcon_write_char(uint8_t ch);

#endif /* DEBUGCON_H */
