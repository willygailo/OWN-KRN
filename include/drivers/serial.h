#ifndef OWNKRN_DRIVERS_SERIAL_H
#define OWNKRN_DRIVERS_SERIAL_H

#include <stdbool.h>
#include <stdint.h>

#define COM1_PORT 0x3F8

void serial_init(void);
void serial_enable_rx_irq(void);
void serial_putc(char c);
void serial_puts(const char *s);
bool serial_has_data(void);
int  serial_getc_nonblock(void);

#endif /* OWNKRN_DRIVERS_SERIAL_H */
