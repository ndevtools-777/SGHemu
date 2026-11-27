#include "rtc.h"
#include <string.h>

#define NS_PER_SECOND UINT64_C(1000000000)

enum { RTC_CONTROL = 6, ALARM_MINUTE = 9, ALARM_YEAR = 13 };

static unsigned leap_year(unsigned year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static unsigned days(unsigned y, unsigned m)
{
    static const unsigned length[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return length[m - 1] + (m == 2 && leap_year(y));
}

static int valid_date(const PhoneRtc *r, const RtcDate *d)
{
    return d->year >= r->year_base && d->year <= r->year_base + 31 && d->month && d->month <= 12 &&
           d->day && d->day <= days(d->year, d->month) && d->hour < 24 && d->minute < 60 && d->second < 60;
}

static void update(PhoneRtc *r)
{
    uint64_t now = *r->nanoseconds, seconds, value, carry;
    RtcDate *d = &r->date;
    if (r->host_sync || now < r->last_ns) {
        r->last_ns = now;
        return;
    }
    seconds = (now - r->last_ns) / NS_PER_SECOND;
    if (!seconds)
        return;
    r->last_ns += seconds * NS_PER_SECOND;
    value = d->second + seconds;
    d->second = (unsigned)(value % 60);
    carry = value / 60;
    value = d->minute + carry;
    d->minute = (unsigned)(value % 60);
    carry = value / 60;
    value = d->hour + carry;
    d->hour = (unsigned)(value % 24);
    carry = value / 24;
    while (carry) {
        unsigned length = days(d->year, d->month), left = d->day < length ? length - d->day : 0;
        if (carry <= left) {
            d->day += (unsigned)carry;
            break;
        }
        carry -= left + 1;
        d->day = 1;
        if (++d->month == 13) {
            d->month = 1;
            if (++d->year > r->year_base + 31)
                d->year = r->year_base;
        }
    }
}

static uint16_t value(PhoneRtc *r, unsigned i)
{
    switch (i) {
    case 0:
        return (uint16_t)r->date.second;
    case 1:
        return (uint16_t)r->date.minute;
    case 2:
        return (uint16_t)r->date.hour;
    case 3:
        return (uint16_t)(r->date.day - 1);
    case 4:
        return (uint16_t)(r->date.month - 1);
    case 5:
        return (uint16_t)(r->date.year - r->year_base);
    default:
        return r->registers[i];
    }
}

static int alarm_matches(PhoneRtc *r)
{
    for (unsigned i = ALARM_MINUTE; i <= ALARM_YEAR; ++i)
        if (value(r, i - 8) != r->registers[i])
            return 0;
    return 1;
}

static uint64_t year_days(unsigned year)
{
    uint64_t y = year - 1;
    return y * 365 + y / 4 - y / 100 + y / 400;
}

static uint64_t calendar_seconds(const RtcDate *d)
{
    uint64_t elapsed_days = year_days(d->year);
    unsigned last_day = days(d->year, d->month);
    for (unsigned month = 1; month < d->month; ++month)
        elapsed_days += days(d->year, month);
    /* Independent guest counter writes can briefly select February 31.
       Its next midnight is March 1, just as for the last valid day. */
    elapsed_days += (d->day > last_day ? last_day : d->day) - 1;
    return ((elapsed_days * 24 + d->hour) * 60 + d->minute) * 60 + d->second;
}

static void raise_alarm(PhoneRtc *r)
{
    if (r->intc)
        r->intc->pending |= RTC_ALARM_IRQ;
    ++r->alarm_events;
}

static void schedule_alarm(PhoneRtc *r)
{
    const uint16_t *regs = r->registers;
    const RtcDate alarm = {r->year_base + regs[ALARM_YEAR], regs[12] + 1u, regs[11] + 1u, regs[10],
                           regs[ALARM_MINUTE], 0};
    r->alarm_due = r->alarm_end = UINT64_MAX;
    if (!valid_date(r, &alarm)) {
        r->alarm_match = 0;
        return;
    }
    int match = alarm_matches(r);
    if (match && !r->alarm_match)
        raise_alarm(r);
    r->alarm_match = match;
    if (!r->host_sync) {
        uint64_t minute_left = (60 - r->date.second) * NS_PER_SECOND;
        if (match && minute_left < UINT64_MAX - r->last_ns)
            r->alarm_end = r->last_ns + minute_left;
        uint64_t now = calendar_seconds(&r->date), target = calendar_seconds(&alarm);
        if (target <= now || (target / 86400 == now / 86400 && alarm.day != r->date.day))
            target += (year_days(r->year_base + 32) - year_days(r->year_base)) * 86400;
        uint64_t delay = (target - now) * NS_PER_SECOND;
        if (delay < UINT64_MAX - r->last_ns)
            r->alarm_due = r->last_ns + delay;
    }
}

void rtc_advance(PhoneRtc *r)
{
    if (!r->attached)
        return;
    int due = r->alarm_due != UINT64_MAX && *r->nanoseconds >= r->alarm_due;
    update(r);
    if (due) {
        /* The deadline also catches an advance across the whole matching minute. */
        raise_alarm(r);
        r->alarm_match = 1;
        schedule_alarm(r);
    } else if (r->alarm_match && !alarm_matches(r)) {
        r->alarm_match = 0;
        r->alarm_end = UINT64_MAX;
    }
}

static int valid(PhoneRtc *r, uint32_t offset, unsigned width, int write, uint32_t v)
{
    if ((width != 1 && width != 2 && width != 4) || offset >= sizeof(r->registers) ||
        width > sizeof(r->registers) - offset) {
        bus_fault_access(r->bus, write, r->base + offset, width, v, "invalid RTC access");
        return 0;
    }
    return 1;
}

static uint32_t read_rtc(void *ctx, uint32_t offset, unsigned width)
{
    PhoneRtc *r = ctx;
    uint32_t v = 0;
    unsigned i;
    if (!valid(r, offset, width, 0, 0))
        return 0;
    rtc_advance(r);
    for (i = 0; i < width; ++i)
        v |= (uint32_t)((value(r, (offset + i) / 2) >> (((offset + i) & 1) * 8)) & 255u) << (i * 8);
    return v;
}

static void write_rtc(void *ctx, uint32_t offset, unsigned width, uint32_t v)
{
    PhoneRtc *r = ctx;
    unsigned i;
    int alarm_changed = 0;
    RtcDate *d = &r->date;
    if (!valid(r, offset, width, 1, v))
        return;
    rtc_advance(r);
    for (i = 0; i < width; ++i) {
        unsigned at = offset + i, index = at / 2;
        uint16_t byte = (uint16_t)((v >> (i * 8)) & 255u);
        if (index < 6) {
            if (r->host_sync || (at & 1))
                continue;
            alarm_changed = 1;
            /* An RTC counter increments on a write; the value is ignored. */
            switch (index) {
            case 0:
                d->second = (d->second + 1) % 60;
                break;
            case 1:
                d->minute = (d->minute + 1) % 60;
                break;
            case 2:
                d->hour = (d->hour + 1) % 24;
                break;
            case 3:
                d->day = d->day % 31 + 1;
                break;
            case 4:
                d->month = d->month % 12 + 1;
                break;
            default:
                d->year = r->year_base + (d->year - r->year_base + 1) % 32;
                break;
            }
        } else {
            unsigned shift = (at & 1) * 8;
            r->registers[index] = (uint16_t)((r->registers[index] & ~(255u << shift)) | (byte << shift));
            if (index == RTC_CONTROL && !(at & 1)) {
                if (!r->host_sync) {
                    *d = (RtcDate){r->year_base, 1, 1, 0, 0, 0};
                    r->last_ns = *r->nanoseconds;
                }
                for (unsigned reg = ALARM_MINUTE; reg <= ALARM_YEAR; ++reg)
                    r->registers[reg] = 0xffff;
                alarm_changed = 1;
            }
            if (index >= ALARM_MINUTE && index <= ALARM_YEAR)
                alarm_changed = 1;
        }
    }
    if (alarm_changed)
        schedule_alarm(r);
}

int rtc_attach(PhoneRtc *r, Bus *bus, uint32_t base, unsigned year_base, const uint64_t *ns)
{
    if (r->attached || (base & 1) || year_base < 1 || year_base > 9968 || !ns)
        return 0;
    memset(r, 0, sizeof(*r));
    r->bus = bus;
    r->base = base;
    r->year_base = year_base;
    r->nanoseconds = ns;
    r->last_ns = *ns;
    r->date.year = year_base;
    r->date.month = r->date.day = 1;
    r->alarm_due = r->alarm_end = UINT64_MAX;
    for (unsigned i = ALARM_MINUTE; i <= ALARM_YEAR; ++i)
        r->registers[i] = 0xffff;
    if (!bus_device(bus, base, sizeof(r->registers), read_rtc, write_rtc, r))
        return 0;
    r->attached = 1;
    return 1;
}

void rtc_connect_irq(PhoneRtc *r, Intc *intc)
{
    if (!r->attached || !intc->attached)
        return;
    rtc_advance(r);
    r->intc = intc;
    schedule_alarm(r);
}

int rtc_set_host_time(PhoneRtc *r, const RtcDate *d)
{
    if (!r->attached || !valid_date(r, d))
        return 0;
    rtc_advance(r);
    r->date = *d;
    r->last_ns = *r->nanoseconds;
    schedule_alarm(r);
    return 1;
}

void rtc_set_sync(PhoneRtc *r, int enabled)
{
    if (!r->attached || r->host_sync == !!enabled)
        return;
    rtc_advance(r);
    r->host_sync = !!enabled;
    r->last_ns = *r->nanoseconds;
    schedule_alarm(r);
}
