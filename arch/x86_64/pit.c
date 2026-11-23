#include <arch/idt.h>
#include <arch/io.h>
#include <arch/pit.h>
#include <drivers/console.h>
#include <stddef.h>

#define PIT_BASE_FREQ 1193182U
#define PIT_CH0_DATA  0x40
#define PIT_CMD       0x43

static volatile uint64_t g_ticks = 0;
static uint32_t          g_hz = PIT_HZ;
static pit_tick_hook_t   g_tick_hook = NULL;

static void pit_irq_handler(struct interrupt_frame *frame) {
    (void)frame;
    g_ticks++;
    if ((g_ticks % 100ULL) == 0) {
        console_tick_hud(g_ticks);
    }
    if (g_tick_hook) {
        g_tick_hook();
    }
}

void pit_init(uint32_t hz) {
    if (hz == 0) {
        hz = PIT_HZ;
    }
    g_hz = hz;
    uint32_t divisor = PIT_BASE_FREQ / hz;
    if (divisor < 1) divisor = 1;
    if (divisor > 65535) divisor = 65535;

    outb(PIT_CMD, 0x36); /* Channel 0, lobyte/hibyte, mode 3 square wave */
    outb(PIT_CH0_DATA, (uint8_t)(divisor & 0xFF));
    outb(PIT_CH0_DATA, (uint8_t)((divisor >> 8) & 0xFF));

    irq_register_handler(0, pit_irq_handler);
}

uint64_t pit_ticks(void) {
    return g_ticks;
}

uint64_t pit_uptime_ms(void) {
    return (g_ticks * 1000ULL) / g_hz;
}

void pit_set_tick_hook(pit_tick_hook_t hook) {
    g_tick_hook = hook;
}
