#ifndef OWNKRN_DRIVERS_CONSOLE_H
#define OWNKRN_DRIVERS_CONSOLE_H

#include <stdint.h>

void console_init(void);
void console_clear(void);
void console_putc(char c);
void console_puts(const char *s);
void console_set_color(uint8_t fg, uint8_t bg);

#endif /* OWNKRN_DRIVERS_CONSOLE_H */
