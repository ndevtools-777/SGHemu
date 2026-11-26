#include "ma3.h"
#include <stdio.h>
#include <string.h>

/* Exercise the native chip through the same byte ports used by the firmware. */
enum { SELECT = 0x200000, DATA = 0x200004 };
static const uint64_t sequence_tick = UINT64_C(865) * 1157;
static Bus bus;
static Ma3 chip;
static int16_t last_sample;

static void select_port(unsigned value) { bus_write(&bus, SELECT, 1, value); }
static void data(unsigned value) { bus_write(&bus, DATA, 1, value); }
static unsigned read_data(void) { return bus_read(&bus, DATA, 1); }

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

static unsigned read_parameter(unsigned address)
{
    select_port(3);
    field(address);
    data(0x80);
    select_port(0x11);
    return read_data();
}

static void upload(unsigned address, const uint8_t *bytes, unsigned length)
{
    unsigned i;
    select_port(2);
    field(address);
    data(length & 127);
    data((length >> 7) | 128);
    for (i = 0; i < length; ++i) data(bytes[i]);
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

static void observe(void *context, int16_t *left, int16_t *right, unsigned frames)
{
    (void)context;
    (void)right;
    if (frames) last_sample = left[frames - 1];
}

static int sample(unsigned format)
{
    static const char *const names[] = {"ADPCM4", "PCM16", "PCM8"};
    static const unsigned pitch[][3] = {{2,0,60}, {2,64,63}, {0x12,0x54,79}};
    static const unsigned cursor[][3] = {{0x42,0x42,0x44}, {0x48,0x48,0x4a}, {0x42,0x42,0x43}};
    uint8_t descriptor[] = {0xfa,0,0,0xf0,0xf0,0,0,0x40,0x20,0,0,1,0}, bytes[512];
    unsigned i, test;
    descriptor[1] = (uint8_t)format;
    for (test = 0; test < sizeof(pitch) / sizeof(*pitch); ++test) {
        unsigned position, readback;
        control(0x80);
        for (i = 0; i < sizeof(bytes); ++i)
            bytes[i] = (uint8_t)(format == 0 ? 0x77 : format == 1 && !(i & 1u) ? 0 : 0x40);
        upload(0x4000, descriptor, sizeof(descriptor));
        upload(0x4020, bytes, sizeof(bytes));
        select_port(7); data(31);
        parameter(0x90, 0x7c);
        parameter(0x60, 0x40); parameter(0x61, 0); parameter(0x62, 0x7c);
        parameter(0x63, pitch[test][0]); parameter(0x64, pitch[test][1]); parameter(0x65, 0x40);
        advance(10000000);
        position = chip.synth.pcm[0].phase >> 16;
        readback = read_parameter(0x14a);
        printf("%s pitch %02x,%02x: %u samples in 10 ms, RBO1=%02x\n", names[format],
               pitch[test][0], pitch[test][1], position, readback);
        if (position != pitch[test][2] || readback != cursor[format][test] || last_sample <= 0) return 0;
    }

    /* Refill while still keyed on. The next lap must play the new data. */
    for (i = 0; i < sizeof(bytes); ++i)
        bytes[i] = (uint8_t)(format == 0 ? 0xff : format == 1 && !(i & 1u) ? 0 : 0xc0);
    upload(0x4020, bytes, sizeof(bytes));
    advance(100000000);
    printf("%s live refill: last sample %d\n", names[format], (int)last_sample);
    return last_sample < 0;
}

static void stream_event(unsigned delay, unsigned address, unsigned value)
{
    select_port(1);
    field(delay);
    field(address);
    data(value | 128);
}

static int sequence_and_timer(void)
{
    const uint64_t refill = UINT64_C(37109) * 27 * 20;
    unsigned irq;
    control(0x80);
    parameter(0x162, 0x7f);
    parameter(0x15e, 6); parameter(0x15f, 97);
    stream_event(10, 0x15a, 0x43);
    stream_event(1000, 0x15b, 0x44);
    parameter(0x161, 1);
    advance(10 * sequence_tick);
    select_port(0);
    irq = read_data();
    if (!(irq & 1) || read_parameter(0x15a) != 0x43) return 0;
    printf("Stream start: IRQ=%02x at %.3f ms\n", irq, (double)(10 * sequence_tick) / 1e6);

    parameter(0x161, 0);
    select_port(0); data(0xff);
    control(0x20);
    stream_event(100, 0x15a, 3);
    control(8);
    parameter(0x161, 1);
    advance(100 * sequence_tick - 1);
    select_port(0);
    if (read_data() & 1) return 0;
    advance(1);
    if (!(read_data() & 1) || read_parameter(0x15a) != 3) return 0;
    printf("Restart: fresh 100-tick event at %.3f ms\n", (double)(100 * sequence_tick) / 1e6);

    parameter(0x152, 27); parameter(0x154, 20); parameter(0x155, 1);
    advance(refill - 1);
    select_port(0);
    if (read_data() & 0x20) return 0;
    advance(1);
    irq = read_data();
    data(0x20);
    printf("Refill timer: IRQ=%02x at %.3f ms\n", irq, (double)refill / 1e6);
    return (irq & 0x20) && !(read_data() & 0x20);
}

int main(int argc, char **argv)
{
    Intc intc = {0};
    unsigned format;
    int ok = 1;
    if (argc > 2) {
        fputs("Usage: ma3pcmprobe [OUTPUT.wav]\n", stderr);
        return 2;
    }
    intc.attached = 1;
    if (!ma3_attach(&chip, &bus, &intc, SELECT, 0x800000, 1157) ||
        !ma3_open_audio(&chip, 0, argc == 2 ? argv[1] : "build/ma3-pcm-probe.wav")) ok = 0;
    if (ok) {
        ma3_set_mixer(&chip, observe, NULL);
        for (format = 0; format < 3; ++format) if (!sample(format)) ok = 0;
        if (!sequence_and_timer() || !ma3_close_audio(&chip) || bus.fault || *chip.error) ok = 0;
    }
    if (!ok) fprintf(stderr, "MA-3 probe failed: %s %s\n", chip.error, chip.output_error);
    ma3_destroy(&chip);
    bus_destroy(&bus);
    return ok ? 0 : 1;
}
