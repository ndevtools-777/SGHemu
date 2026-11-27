#ifndef SGH_RTC_H
#define SGH_RTC_H
#include "intc.h"

enum { RTC_ALARM_IRQ = 1u << 2 };

typedef struct {
    unsigned year, month, day, hour, minute, second;
} RtcDate;

typedef struct {
    Bus *bus;
    Intc *intc;
    const uint64_t *nanoseconds;
    uint64_t last_ns, alarm_due, alarm_end, alarm_events;
    uint32_t base;
    unsigned year_base;
    RtcDate date;
    uint16_t registers[32];
    int attached, host_sync, alarm_match;
} PhoneRtc;

int rtc_attach(PhoneRtc *rtc, Bus *bus, uint32_t base, unsigned year_base, const uint64_t *nanoseconds);
void rtc_connect_irq(PhoneRtc *rtc, Intc *intc);
void rtc_advance(PhoneRtc *rtc);
int rtc_set_host_time(PhoneRtc *rtc, const RtcDate *date);
void rtc_set_sync(PhoneRtc *rtc, int enabled);
#endif
