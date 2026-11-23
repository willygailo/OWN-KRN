#ifndef OWNKRN_DRIVERS_TTY_H
#define OWNKRN_DRIVERS_TTY_H

#include <kernel/types.h>

void     tty_init(void);
void     tty_rx_char(char c);
ssize_t  tty_read(void *buf, size_t len);
ssize_t  tty_write(const void *buf, size_t len);
void     tty_set_mode(uint32_t mode);
uint32_t tty_get_mode(void);

#endif /* OWNKRN_DRIVERS_TTY_H */
