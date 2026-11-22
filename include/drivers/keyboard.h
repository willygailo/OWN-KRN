#ifndef OWNKRN_DRIVERS_KEYBOARD_H
#define OWNKRN_DRIVERS_KEYBOARD_H

#include <stdbool.h>

typedef void (*input_sink_t)(char c);

void keyboard_init(void);
void keyboard_set_sink(input_sink_t sink);
int  keyboard_getc_nonblock(void);

#endif /* OWNKRN_DRIVERS_KEYBOARD_H */
