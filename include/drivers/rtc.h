#ifndef OWNKRN_DRIVERS_RTC_H
#define OWNKRN_DRIVERS_RTC_H

#include <stdint.h>

struct rtc_time {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
};

void    rtc_read(struct rtc_time *out);
int64_t rtc_unix_seconds(void);

#endif /* OWNKRN_DRIVERS_RTC_H */
