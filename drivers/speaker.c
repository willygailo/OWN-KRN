#include <arch/io.h>
#include <drivers/speaker.h>
#include <kernel/sched.h>

#define PIT_BASE_FREQ 1193182U
#define PIT_CH2_DATA  0x42
#define PIT_CMD       0x43
#define PC_SPEAKER_IO 0x61

void speaker_beep(uint32_t freq_hz, uint32_t duration_ms) {
    if (freq_hz < 40) freq_hz = 40;
    if (freq_hz > 20000) freq_hz = 20000;
    if (duration_ms < 5) duration_ms = 5;
    if (duration_ms > 1000) duration_ms = 1000;

    uint32_t div = PIT_BASE_FREQ / freq_hz;
    if (div < 1) div = 1;
    if (div > 65535) div = 65535;

    outb(PIT_CMD, 0xB6); /* Channel 2, lobyte/hibyte, mode 3 square wave */
    outb(PIT_CH2_DATA, (uint8_t)(div & 0xFFU));
    outb(PIT_CH2_DATA, (uint8_t)((div >> 8) & 0xFFU));

    uint8_t tmp = inb(PC_SPEAKER_IO);
    if ((tmp & 0x03U) != 0x03U) {
        outb(PC_SPEAKER_IO, (uint8_t)(tmp | 0x03U));
    }

    sleep_ms(duration_ms);

    outb(PC_SPEAKER_IO, (uint8_t)(inb(PC_SPEAKER_IO) & ~0x03U));
}
