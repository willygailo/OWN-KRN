#ifndef OWNKRN_ARCH_PIT_H
#define OWNKRN_ARCH_PIT_H

#include <stdint.h>

#define PIT_HZ 100U

typedef void (*pit_tick_hook_t)(void);

void     pit_init(uint32_t hz);
uint64_t pit_ticks(void);
uint64_t pit_uptime_ms(void);
void     pit_set_tick_hook(pit_tick_hook_t hook);

#endif /* OWNKRN_ARCH_PIT_H */
