#ifndef OWNKRN_DRIVERS_SERIAL_H
#define OWNKRN_DRIVERS_SERIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define COM1_PORT 0x3F8
#define COM2_PORT 0x2F8

void   serial_init(void);
void   serial_enable_rx_irq(void);
void   serial_putc(char c);
void   serial_puts(const char *s);
bool   serial_has_data(void);
int    serial_getc_nonblock(void);

/* COM2 Host Clipboard Bridge */
size_t serial_clipboard_sync_from_host(char *dst, size_t max_len);
void   serial_clipboard_send_to_host(const char *src, size_t len);

#endif /* OWNKRN_DRIVERS_SERIAL_H */
