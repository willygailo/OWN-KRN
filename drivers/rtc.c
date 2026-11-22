#include <arch/io.h>
#include <drivers/rtc.h>
#include <stdbool.h>

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static bool cmos_updating(void) {
    outb(CMOS_ADDR, 0x0A);
    return (inb(CMOS_DATA) & 0x80) != 0;
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

static void rtc_read_raw(struct rtc_time *t) {
    while (cmos_updating()) {
        cpu_pause();
    }
    t->second = cmos_read(0x00);
    t->minute = cmos_read(0x02);
    t->hour   = cmos_read(0x04);
    t->day    = cmos_read(0x07);
    t->month  = cmos_read(0x08);
    t->year   = cmos_read(0x09);
}

void rtc_read(struct rtc_time *out) {
    struct rtc_time a, b;
    uint64_t flags = irq_save();
    do {
        rtc_read_raw(&a);
        rtc_read_raw(&b);
    } while (a.second != b.second || a.minute != b.minute ||
             a.hour != b.hour || a.day != b.day ||
             a.month != b.month || a.year != b.year);
    uint8_t reg_b = cmos_read(0x0B);
    irq_restore(flags);

    if (!(reg_b & 0x04)) {
        a.second = bcd_to_bin(a.second);
        a.minute = bcd_to_bin(a.minute);
        a.hour   = (uint8_t)(bcd_to_bin(a.hour & 0x7F) | (a.hour & 0x80));
        a.day    = bcd_to_bin(a.day);
        a.month  = bcd_to_bin(a.month);
        a.year   = bcd_to_bin((uint8_t)a.year);
    }
    if (!(reg_b & 0x02) && (a.hour & 0x80)) {
        a.hour = (uint8_t)(((a.hour & 0x7F) + 12) % 24);
    }
    a.year = (uint16_t)(2000 + a.year);
    *out = a;
}

static bool is_leap(uint32_t y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

int64_t rtc_unix_seconds(void) {
    static const uint16_t mdays[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    struct rtc_time t;
    rtc_read(&t);

    int64_t days = 0;
    for (uint32_t y = 1970; y < t.year; y++) {
        days += is_leap(y) ? 366 : 365;
    }
    for (uint32_t m = 1; m < t.month && m <= 12; m++) {
        days += mdays[m - 1];
        if (m == 2 && is_leap(t.year)) {
            days += 1;
        }
    }
    if (t.day > 0) {
        days += (int64_t)(t.day - 1);
    }
    return days * 86400LL + (int64_t)t.hour * 3600LL +
           (int64_t)t.minute * 60LL + (int64_t)t.second;
}
