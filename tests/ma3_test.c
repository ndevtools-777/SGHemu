#include "ma3.h"
#include <stdlib.h>
#include <string.h>

enum { SELECT = 0x200000, DATA = 0x200004, IRQ = 0x800000 };

static const uint64_t sequence_tick = UINT64_C(865) * 1157;
static Bus bus;
static Intc intc;
static Ma3 chip;
static unsigned checks;

#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        ++checks;                                                                                            \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "MA-3 line %d: %s\n", __LINE__, #x);                                             \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)

static void select_port(unsigned value)
{
    bus_write(&bus, SELECT, 1, value);
}

static void data(unsigned value)
{
    bus_write(&bus, DATA, 1, value);
}

static unsigned read_data(void)
{
    return bus_read(&bus, DATA, 1);
}

static unsigned status(void)
{
    return bus_read(&bus, SELECT, 1);
}

static void field(unsigned value)
{
    while (value >= 128) {
        data(value & 127);
        value >>= 7;
    }
    data(value | 128);
}

static void parameter(unsigned address, unsigned value)
{
    select_port(2);
    field(address);
    data(value | 128);
}

static void delayed(unsigned ticks, unsigned address, unsigned value)
{
    select_port(1);
    field(ticks);
    field(address);
    data(value | 128);
}

static unsigned read_parameter(unsigned address, unsigned buffer)
{
    static const unsigned selectors[] = {0x11, 0x22, 0x43};
    unsigned value, ready = 0x10u << buffer;
    select_port(3);
    field(address);
    data(0x80 + buffer);
    CHECK(status() & ready);
    select_port(selectors[buffer]);
    value = read_data();
    CHECK(!(status() & ready));
    return value;
}

static void ram(unsigned address, const uint8_t *bytes, unsigned length)
{
    unsigned i;
    select_port(2);
    field(address);
    data(length & 127);
    data((length >> 7) | 128);
    for (i = 0; i < length; ++i)
        data(bytes[i]);
}

static void control(unsigned bits)
{
    select_port(4);
    data(bits);
    data(0);
}

static void advance(uint64_t ns)
{
    bus.ticks += ns;
    ma3_advance(&chip, bus.ticks);
}

static void divider(void)
{
    parameter(0x15e, 6);
    parameter(0x15f, 97);
}

static void ports(void)
{
    const uint8_t literal[] = {0x80, 0xff};
    CHECK(read_parameter(0x90, 2) == 0x60);
    CHECK(read_parameter(0xa0, 1) == 0x3c);
    CHECK(read_parameter(0x14a, 2) == 0x40);
    ram(0x4020, literal, sizeof(literal));
    CHECK(read_parameter(0x4020, 0) == 0x80);
    CHECK(read_parameter(0x4021, 0) == 0xff);

    /* Zero-delay commands can span selector writes and take effect at the
       same emulated time as the run command. */
    select_port(1);
    data(0x80);
    data(0x83);
    select_port(0x81);
    data(0xac);
    CHECK(!(status() & 2) && read_parameter(3, 0) == 0);
    parameter(0x161, 1);
    CHECK(chip.stats.frames == 0 && (status() & 2));
    CHECK(read_parameter(3, 0) == 0x2c);
    delayed(0, 0x119, 0x24);
    CHECK(read_parameter(0x119, 0) == 0x24 && chip.stats.frames == 0);
    bus_write(&bus, DATA, 2, 1);
    CHECK(bus.fault);
    bus.fault = 0;

    control(0x80);
    CHECK(read_parameter(3, 0) == 0 && read_parameter(0x4021, 0) == 0);

    select_port(0x0e);
    data(4);
    select_port(0x0d);
    data(1);
    CHECK(ma3_motor_on(&chip) && !chip.led_on);
    select_port(4);
    data(1); /* An unrelated bank cannot change the motor. */
    select_port(0x0d);
    data(0);
    CHECK(ma3_motor_on(&chip));
    select_port(4);
    data(0);
    select_port(0x0d);
    data(0);
    CHECK(!ma3_motor_on(&chip));
    parameter(0x147, 1); /* Also sent by the firmware's motor-off path. */
    CHECK(!ma3_motor_on(&chip));
    select_port(0x0d);
    data(1);
    control(0x80);
    CHECK(!ma3_motor_on(&chip));
}

static void sequence(void)
{
    uint64_t led_events = chip.led_events, drops = chip.partial_drops;
    divider();
    delayed(100, 0x146, 1);
    delayed(10, 0x146, 0);
    parameter(0x161, 1);
    advance(30 * sequence_tick);
    parameter(0x161, 0);
    advance(200 * sequence_tick);
    CHECK(chip.led_events == led_events);
    parameter(0x161, 1);
    advance(70 * sequence_tick - 1);
    CHECK(!chip.led_on && chip.led_events == led_events);
    advance(1);
    CHECK(chip.led_on && chip.led_events == led_events + 1);
    CHECK(chip.led_tick == bus.ticks && chip.led_color >= 1 && chip.led_color <= 4);
    advance(10 * sequence_tick);
    CHECK(!chip.led_on && chip.led_events == led_events + 2);

    /* Clear drops a partial packet. An explicit reset rebases the delay of
       the first fresh packet, even when the sequencer is already running. */
    select_port(1);
    data(0x8a);
    data(0x5a);
    control(0x28);
    CHECK(chip.partial_drops == drops + 1 && (status() & 2));
    delayed(100, 0x15a, 0x43);
    delayed(1000, 0x15a, 0x44);
    advance(30 * sequence_tick);
    control(8);
    advance(100 * sequence_tick - 1);
    CHECK(read_parameter(0x15a, 0) == 0);
    advance(1);
    CHECK(read_parameter(0x15a, 0) == 0x43);
    parameter(0x161, 0);
    control(0x20);
    advance(1000 * sequence_tick);
    CHECK(read_parameter(0x15a, 0) == 0x43 && (status() & 2));
}

static void interrupts(void)
{
    unsigned i;
    const uint64_t timer_a_period = UINT64_C(37109) * 27 * 20;
    control(0x80);
    divider();
    parameter(0x162, 0);
    parameter(0x15b, 0x44);
    select_port(0x80);
    CHECK(!(read_data() & 2) && !intc.pending);
    parameter(0x162, 2);
    select_port(0);
    CHECK((read_data() & 2) && !intc.pending);
    select_port(0x80);
    CHECK(intc.pending == IRQ && read_data() == 2);
    data(2);
    intc.pending = 0; /* The CPU acknowledges its interrupt controller separately. */
    CHECK(!read_data());

    parameter(0x160, 1);
    for (i = 0; i < 17; ++i)
        delayed(10, 0x166, 0);
    advance(2000000);
    select_port(0x80);
    CHECK(!read_data() && !intc.pending); /* Still above the 64-byte threshold. */
    parameter(0x161, 1);
    select_port(0x81);
    advance(10 * sequence_tick);
    CHECK(intc.pending == IRQ);
    select_port(0);
    CHECK(read_data() == 0x80);
    data(0x80);
    intc.pending = 0;
    CHECK(!read_data());

    parameter(0x162, 0x60);
    parameter(0x152, 27);
    parameter(0x154, 20);
    parameter(0x157, 5);
    parameter(0x155, 1);
    parameter(0x158, 5); /* Timer B uses the sequencer divider in this mode. */
    advance(5 * sequence_tick);
    select_port(0);
    CHECK((read_data() & 0x60) == 0x40);
    data(0x40);
    parameter(0x158, 0);
    advance(timer_a_period - 5 * sequence_tick - 1);
    select_port(0);
    CHECK(!(read_data() & 0x60));
    advance(1);
    CHECK((read_data() & 0x60) == 0x20);
    data(0x20);
    parameter(0x155, 0);
    advance(timer_a_period);
    select_port(0);
    CHECK(!(read_data() & 0x60));
}

static void playback(void)
{
    /* ADPCM: 63 samples in 10 ms. RBO1 reports the byte containing sample 63;
       RBO3 still returns the reset value. Muted runs must advance the cursor. */
    const uint8_t descriptor[] = {0xfa, 0, 0, 0xf0, 0xf0, 0, 0, 0x40, 0x20, 0, 0, 1, 0};
    const uint8_t fm[] = {0x78, 0x45, 0, 0, 0, 0, 0, 0, 0, 2, 0xf0, 0xf0, 0, 0, 0x10,
                          0,    0,    0, 0, 0, 0, 0, 0, 0, 0, 0,    0,    0, 0, 0};
    control(0x80);
    ram(0x4000, descriptor, sizeof(descriptor));
    parameter(0x60, 0x40);
    parameter(0x61, 0);
    parameter(0x63, 2);
    parameter(0x64, 0x40);
    parameter(0x65, 0x40);
    advance(10000000);
    CHECK(chip.synth.pcm[0].phase >> 16 == 63);
    CHECK(read_parameter(0x14a, 0) == 0x42 && read_parameter(0x14a, 2) == 0x40);

    control(0x80);
    CHECK(ma3_open_audio(&chip, 0, NULL));
    select_port(7);
    data(31);
    ram(0x4000, fm, sizeof(fm));
    /* Algorithm 5 has two independent pairs; operator 2 is a carrier.
       Exercise the second FM bank with just that carrier enabled. */
    parameter(0xe0, 0x40);
    parameter(0xe1, 0);
    parameter(0xe2, 0x7c);
    parameter(0xe3, 0x2c);
    parameter(0xe4, 0x4a);
    parameter(0xe5, 0x40);
    advance(10000000);
    CHECK(chip.stats.frames == 480 && chip.stats.nonzero > 500 && chip.stats.peak > 0);
    parameter(0xe5, 0);
    advance(10000000);
    {
        uint64_t nonzero = chip.stats.nonzero;
        advance(10000000);
        CHECK(chip.stats.nonzero == nonzero);
    }
    CHECK(ma3_close_audio(&chip));

    /* Water may damage payload data, never the address or length. */
    ma3_set_water(&chip, 1);
    chip.water_sample_burst = 4;
    ram(0x4020, descriptor, 4);
    CHECK(read_parameter(0x4020, 0) != descriptor[0]);
    CHECK(read_parameter(0x4024, 0) == 0);
    advance(100000000);
    CHECK(chip.water_faults >= 5);
    ma3_set_water(&chip, 0);
    CHECK(!chip.water && !chip.water_pcm.enabled);
}

static void limits(void)
{
    unsigned i;
    control(0x80);
    select_port(1);
    for (i = 0; i < MA3_FIFO_BYTES / 4; ++i) {
        data(0x80);
        field(0x166);
        data(0x80);
    }
    CHECK((status() & 8) && !*chip.error);
    data(0x80);
    CHECK(strstr(chip.error, "FIFO overflow") != NULL);
}

int main(void)
{
    intc.attached = 1;
    CHECK(!ma3_attach(&chip, &bus, &intc, SELECT, IRQ, 0));
    CHECK(ma3_attach(&chip, &bus, &intc, SELECT, IRQ, 1157));
    CHECK(!ma3_attach(&chip, &bus, &intc, SELECT, IRQ, 1157));
    ports();
    sequence();
    interrupts();
    playback();
    limits();
    ma3_destroy(&chip);
    bus_destroy(&bus);

    /* A legacy clock cannot round each individual advance to a frame. */
    bus.ticks = 0;
    CHECK(ma3_attach(&chip, &bus, &intc, SELECT, IRQ, 1157));
    ma3_set_clock(&chip, &bus.ticks, 23400000);
    while (bus.ticks < 2340000) {
        advance(137);
        CHECK(chip.stats.frames == bus.ticks * AUDIO_RATE / 23400000);
    }
    ma3_destroy(&chip);
    bus_destroy(&bus);
    printf("MA-3: %u checks passed\n", checks);
    return 0;
}
