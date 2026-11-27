/* Compare deadline batches with ordinary steps at randomized boundaries. */
#include "machine.h"
#include <stddef.h>
#include <stdlib.h>
static unsigned checks, round_number;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        ++checks;                                                                                            \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "Machine batch round %u line %d: %s\n", round_number, __LINE__, #x);             \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)
static Machine *a, *b;

static void both(uint32_t address, unsigned width, uint32_t value)
{
    bus_write(&a->bus, address, width, value);
    bus_write(&b->bus, address, width, value);
}

static void fixture(void)
{
    uint8_t code[512] = {0};
    FILE *f;
    const uint16_t loop[] = {0x6801, 0x3101, 0x6001, 0x781a, 0x701a, 0x4351, 0xe7f8};
    const uint16_t undefined[] = {0x46c0, 0xde00, 0xe7fe}; /* NOP; undefined; B . */
    const uint32_t sleep[] = {0xe1c010b0, 0xeafffffe}; /* STRH r1, [r0]; B . */
    const uint32_t handler[] = {0xe59fc008, 0xe3e0b000, 0xe58cb000, 0xe25ef004, 0x40100};
    const uint32_t undef_return = 0xe1b0f00e; /* MOVS pc, lr */
    const uint32_t abort_spin = 0xeafffffe;    /* B . */
    const uint32_t nop = 0xe1a00000;
    uint32_t irq = 0xea000008, fiq = 0xea000007;
    memcpy(code + 4, &undef_return, 4);
    memcpy(code + 0x0c, &abort_spin, 4);
    memcpy(code + 0x10, &abort_spin, 4);
    memcpy(code + 0x18, &irq, 4);
    memcpy(code + 0x1c, &fiq, 4);
    memcpy(code + 0x40, handler, sizeof(handler));
    memcpy(code + 0x100, loop, sizeof(loop));
    memcpy(code + 0x120, undefined, sizeof(undefined));
    memcpy(code + 0x140, sleep, sizeof(sleep));
    for (unsigned i = 0x160; i < sizeof(code); i += 4)
        memcpy(code + i, &nop, sizeof(nop));
    f = fopen("build/batch-fixture.bin", "wb");
    CHECK(f);
    CHECK(fwrite(code, 1, sizeof(code), f) == sizeof(code));
    CHECK(!fclose(f));
    f = fopen("build/batch-fixture.cfg", "w");
    CHECK(f);
    fputs("rom=0,batch-fixture.bin\nram=0x600000,0x10000\nreset=0x100\naudio=silent\ndisplay=128,128\n"
          "intc=0x40100\nperiodic_irq=26,71\nintc_aux=0x41700\n"
          "clock=0x40b00,3900000\nclock_control=0x40902,0\nlp_timer=0x40700,32768,4,25,4615385\n"
          "register32=0x4090c,8\nirq_gate=0x4090c,8\n"
          "rtc=0x40080,1996\nuart=0x40200,debug\ntimer=0x41500,1\ntimer=0x41400,2\n"
          "exceptions=arm7\nbus_abort=0x610000,0x100\n"
           "sim=0x40a80,0x41300,0x40520\nsds_dma=0x40420,8,17,11\n"
           "gpio_port=0x4029c,0x80\ncharger_complete=0x4029c,2,7,1,2,1\n"
           "lcd_s6b33=0xc00000,0xc80000,0\ngpio_port=0x4028c,0\ngpio_led=0x4028c,5,3,6\nc100_backlights=1\n"
           "irom_service=0x8077c,memcpy\n",
          f);
    CHECK(!fclose(f));
    a = calloc(1, sizeof(*a));
    b = calloc(1, sizeof(*b));
    CHECK(a && b);
    CHECK(machine_load(a, "build/batch-fixture.cfg", NULL, NULL));
    CHECK(machine_load(b, "build/batch-fixture.cfg", NULL, NULL));
    a->cpu.cpsr = b->cpu.cpsr = 0x33;
    a->cpu.r[0] = b->cpu.r[0] = 0x600000;
    a->cpu.r[3] = b->cpu.r[3] = 0x600004;
}

static void compare(void)
{
    CHECK(!memcmp(&a->cpu, &b->cpu, offsetof(Arm7, bus)));
    CHECK(!memcmp(&a->clock.base, &b->clock.base, sizeof(PhoneClock) - offsetof(PhoneClock, base)));
    CHECK(a->bus.pc == b->bus.pc && a->bus.ticks == b->bus.ticks && a->bus.faults == b->bus.faults);
    CHECK(a->bus.device_epoch == b->bus.device_epoch && a->bus.fault == b->bus.fault);
    CHECK(!memcmp(a->bus.regions[1].data, b->bus.regions[1].data, 0x10000));
    CHECK(!memcmp(&a->intc.base, &b->intc.base, offsetof(Intc, secondary) - offsetof(Intc, base)));
    CHECK(!memcmp(&a->intc_aux.base, &b->intc_aux.base, offsetof(Intc, secondary) - offsetof(Intc, base)));
    CHECK(!memcmp(&a->lp_timer.base, &b->lp_timer.base, sizeof(LpTimer) - offsetof(LpTimer, base)));
    for (unsigned i = 0; i < 2; ++i)
        CHECK(!memcmp(&a->timers[i].base, &b->timers[i].base, offsetof(Gpt, output_changed) - offsetof(Gpt, base)));
    CHECK(a->debug_uart.tx_due == b->debug_uart.tx_due && a->debug_uart.transmits == b->debug_uart.transmits);
    CHECK(a->debug_uart.tx_request == b->debug_uart.tx_request &&
          a->debug_uart.shift_byte == b->debug_uart.shift_byte &&
          a->debug_uart.pin_transmits == b->debug_uart.pin_transmits);
    CHECK(a->rtc.alarm_due == b->rtc.alarm_due && a->rtc.alarm_events == b->rtc.alarm_events &&
          a->rtc.alarm_match == b->rtc.alarm_match && a->rtc.last_ns == b->rtc.last_ns);
    CHECK(!memcmp(&a->rtc.date, &b->rtc.date, sizeof(RtcDate)));
    CHECK(!memcmp(&a->sim.base, &b->sim.base, sizeof(Sim) - offsetof(Sim, base)));
    CHECK(a->sds_dma.next_event == b->sds_dma.next_event && a->sds_dma.transfers == b->sds_dma.transfers);
    CHECK(!memcmp(&a->sds_dma.channel[0].base, &b->sds_dma.channel[0].base,
                  sizeof(SdsDmaChannel) - offsetof(SdsDmaChannel, base)));
    CHECK(a->battery.complete_due == b->battery.complete_due && a->battery.complete == b->battery.complete);
    CHECK(a->phone_io.tone_changes == b->phone_io.tone_changes);
    CHECK(a->irom.services[0].calls == b->irom.services[0].calls);
}

static void same_steps(unsigned count)
{
    for (unsigned n = 0; n < count; ++n)
        CHECK(machine_step(a, 0));
    CHECK(machine_run(b, count, 0) == count);
    compare();
}

static void check_a7(int high)
{
    CHECK(!!(bus_read(&a->bus, 0x4028c, 2) & 0x80) == high);
    CHECK(!!(bus_read(&b->bus, 0x4028c, 2) & 0x80) == high);
    CHECK(!((a->intc.pending | b->intc.pending) & 2));
    CHECK(a->lcd.light_gain == b->lcd.light_gain);
}

static void cx805_timer_pulse(void)
{
    CHECK(machine_load(a, "build/batch-fixture.cfg", NULL, NULL));
    CHECK(machine_load(b, "build/batch-fixture.cfg", NULL, NULL));
    a->cpu.r[15] = b->cpu.r[15] = 0x160;
    both(0x40d04, 2, 0x4000); /* A7 = Timer 2 pulse output. */
    both(0x41400, 4, 4);
    both(0x41408, 4, 3);
    both(0x4140c, 2, 5); /* Pulse mode, ARM clock, IRQ disabled. */
    check_a7(0);
    CHECK(a->lcd.backlight == LCD_BACKLIGHT_OFF);
    both(0x40902, 2, 0x20);
    CHECK(a->lcd.backlight == LCD_BACKLIGHT_PWM);
    unsigned gain = a->lcd.light_gain;
    same_steps(2);
    check_a7(0);
    same_steps(1);
    check_a7(1);
    same_steps(1);
    check_a7(0);
    same_steps(3);
    check_a7(1);
    CHECK(a->lcd.light_gain == gain); /* Panel uses average duty, pin uses phase. */

    both(0x40902, 2, 0);
    check_a7(0);
    CHECK(a->lcd.backlight == LCD_BACKLIGHT_OFF);
    same_steps(5);
    CHECK(bus_read(&a->bus, 0x41408, 4) == 4 && bus_read(&b->bus, 0x41408, 4) == 4);
    both(0x40902, 2, 0x20);
    same_steps(3);
    check_a7(0);
    same_steps(1);
    check_a7(1);
    both(0x40d04, 2, 0);
    check_a7(0);
    both(0x40d04, 2, 0x4000);
    check_a7(1);
    both(0x4140c, 2, 13); /* Stop. */
    check_a7(0);
    CHECK(a->lcd.backlight == LCD_BACKLIGHT_OFF);
}

static void cx805_boundaries(void)
{
    char status[256];
    CHECK(machine_load(a, "build/batch-fixture.cfg", NULL, NULL));
    CHECK(machine_load(b, "build/batch-fixture.cfg", NULL, NULL));
    a->cpu.cpsr = b->cpu.cpsr = ARM_T | ARM_I | ARM_F | 0x13;
    a->cpu.r[0] = b->cpu.r[0] = 0x600000;
    a->cpu.r[3] = b->cpu.r[3] = 0x600004;

    both(0x40902, 2, 0x0220);
    both(0x40202, 2, 3);
    both(0x40200, 2, 1);
    both(0x40204, 2, 'U');
    /* The fixture uses a spare address for Timer 1; SIM owns its stock window. */
    both(0x41500, 4, 12);
    both(0x41508, 4, 4);
    both(0x4150c, 2, 0x10);
    both(0x41400, 4, 12);
    both(0x41408, 4, 4);
    both(0x4140c, 2, 0x14); /* Timer 2 counts ARM cycles with CKS set. */
    CHECK(a->debug_uart.transmits == 0 && (a->intc.pending & 3) == 0);
    same_steps(120);
    CHECK(a->debug_uart.transmits == 1 && a->timers[0].expirations > 0 && a->timers[1].expirations > 0);
    CHECK(a->debug_uart.pin_transmits == 1);
    CHECK((a->intc.pending & 3) == 3);
    both(0x4150c, 2, 8);
    both(0x4140c, 2, 8);
    both(0x40100, 4, UINT32_MAX);

    sds_uart_set_tx_pin(&a->debug_uart, 0);
    sds_uart_set_tx_pin(&b->debug_uart, 0);
    both(0x40204, 2, 'X');
    same_steps(120);
    CHECK(sds_uart_tx_status(&a->debug_uart, "DEBUG", status, sizeof(status)) > 0);
    CHECK(strstr(status, "TX pin=unrouted") && strstr(status, "frames=2, pin bytes=1"));

    a->cpu.r[15] = b->cpu.r[15] = 0x120;
    same_steps(16);
    CHECK(a->cpu.undefined_count == 1 && a->cpu.r[15] == 0x124);
    CHECK(arm7_fault_status(&a->cpu, status, sizeof(status)) > 0);
    CHECK(strstr(status, "last undefined at PC=00000122, opcode=0000de00"));
    a->cpu.r[15] = b->cpu.r[15] = 0x100;
    a->cpu.r[0] = b->cpu.r[0] = 0x610000;
    same_steps(8);
    CHECK(a->cpu.data_abort_count == 1 && a->cpu.r[15] == 0x10);
    CHECK(arm7_fault_status(&a->cpu, status, sizeof(status)) > 0);
    CHECK(strstr(status, "last data abort at PC=00000100, address=00610000"));
    a->cpu.r[15] = b->cpu.r[15] = 0x610000;
    same_steps(8);
    CHECK(a->cpu.prefetch_abort_count == 1 && a->cpu.r[15] == 0x0c);
    char stepped[256];
    CHECK(arm7_fault_status(&a->cpu, status, sizeof(status)) > 0);
    CHECK(arm7_fault_status(&b->cpu, stepped, sizeof(stepped)) > 0);
    CHECK(!strcmp(status, stepped));
    CHECK(strstr(status, "last prefetch abort at PC=00610000, address=00610000"));

    both(0x40108, 4, 1u << 25);
    both(0x4090c, 4, 0); /* A closed IRQ gate keeps the pending wake latched. */
    both(0x40700, 2, 2);
    both(0x40704, 4, (uint32_t)(a->clock.nanoseconds * 32768 / 1000000000) + 54);
    a->cpu.cpsr = b->cpu.cpsr = ARM_F | 0x13;
    a->cpu.r[0] = b->cpu.r[0] = 0x40902;
    a->cpu.r[1] = b->cpu.r[1] = 0x0204;
    a->cpu.r[15] = b->cpu.r[15] = 0x140;
    same_steps(1);
    CHECK(machine_sleeping(a) && a->cpu.r[15] == 0x144);
    uint64_t instructions = a->cpu.instructions, clocks = a->cpu.clock_cycles, ns = a->clock.nanoseconds;
    same_steps(3);
    CHECK(machine_sleeping(a) && a->lp_timer.wake_events == 1 && (a->intc.pending & (1u << 25)));
    CHECK(a->cpu.instructions == instructions && a->cpu.clock_cycles == clocks && a->clock.nanoseconds > ns);
    both(0x4090c, 4, 8);
    same_steps(1);
    CHECK(!machine_sleeping(a) && a->cpu.instructions == instructions);
    same_steps(1);
    CHECK(a->cpu.r[15] == 0x18);
    same_steps(8);
    CHECK(a->cpu.r[15] == 0x144 && !a->intc.pending);

    const RtcDate date = {1996, 1, 1, 0, 0, 59};
    CHECK(rtc_set_host_time(&a->rtc, &date) && rtc_set_host_time(&b->rtc, &date));
    both(0x40092, 2, 1);
    for (uint32_t reg = 0x40094; reg <= 0x4009a; reg += 2)
        both(reg, 2, 0);
    both(0x40108, 4, RTC_ALARM_IRQ);
    a->cpu.cpsr |= ARM_I;
    b->cpu.cpsr |= ARM_I;
    both(0x40902, 2, 0x0204);
    instructions = a->cpu.instructions;
    same_steps(999);
    CHECK(machine_sleeping(a) && a->cpu.instructions == instructions && !a->rtc.alarm_events);
    same_steps(8);
    CHECK(!machine_sleeping(a) && a->cpu.instructions > instructions && a->cpu.r[15] == 0x144);
    CHECK(a->rtc.alarm_events == 1 && (a->intc.pending & RTC_ALARM_IRQ));
}

int main(void)
{
    uint32_t rng = 0xc100805;
    unsigned i, n;
    fixture();
    CHECK(machine_run(b, 0, 1) == 0);
    both(0x40108, 4, 0x06000018);
    both(0x40a80, 2, 11);
    both(0x40a86, 2, 0x18);
    for (round_number = 0; round_number < 4000; ++round_number) {
        rng = rng * 1664525u + 1013904223u;
        n = 1 + (rng & 255);
        if (round_number % 19 == 0) {
            both(0x41300, 4, 1 + (rng & 31));
            both(0x4130c, 4, 8);
        }
        if (round_number % 23 == 0)
            both(0x40a88, 1, rng & 255);
        if (round_number % 29 == 0)
            both(0x40100, 4, UINT32_MAX);
        if (round_number % 31 == 0)
            both(0x40108, 4, (rng & 256) ? 0 : 0x06000018);
        if (round_number % 37 == 0) {
            a->cpu.r[0] = b->cpu.r[0] = (rng & 512) ? 0x40100 : 0x600000;
        }
        if (round_number % 41 == 0) {
            a->sim.present = b->sim.present = !a->sim.present;
        }
        if (round_number % 43 == 0) {
            sim_card_reset(&a->sim.card);
            sim_card_reset(&b->sim.card);
        }
        if (round_number % 47 == 0) {
            a->cpu.r[3] = b->cpu.r[3] = (rng & 1024) ? 0x40a8a : 0x600004;
        }
        if (round_number % 53 == 0) {
            both(0x40700, 2, 2);
            both(0x40704, 4, rng & 0x3ffff);
        }
        if (round_number % 59 == 0) {
            a->clock.arm_hz = b->clock.arm_hz = 1 + (rng % 52000000);
        }
        if (round_number % 61 == 0) {
            battery_set(&a->battery, 100, 1);
            battery_set(&b->battery, 100, 1);
            both(0x4029c, 2, 0x80);
        }
        if (round_number % 67 == 0) {
            battery_set(&a->battery, 25, 0);
            battery_set(&b->battery, 25, 0);
        }
        if (round_number % 71 == 0) {
            a->cpu.r[15] = b->cpu.r[15] = 0x100;
            a->cpu.cpsr = b->cpu.cpsr = 0x33;
            a->cpu.r[0] = b->cpu.r[0] = 0x600000;
            a->cpu.r[3] = b->cpu.r[3] = 0x600004;
            a->phone_io.configured = b->phone_io.configured = 1;
            a->phone_io.tone_stop = b->phone_io.tone_stop = (rng & 2048) ? 0x102 : 0;
        }
        if (round_number % 73 == 0) {
            a->cpu.r[15] = b->cpu.r[15] = 0x8077c;
            a->cpu.cpsr = b->cpu.cpsr = 0xf3;
            a->cpu.r[0] = b->cpu.r[0] = 0x600000;
            a->cpu.r[1] = b->cpu.r[1] = 0x600100;
            a->cpu.r[2] = b->cpu.r[2] = 16;
            a->cpu.r[14] = b->cpu.r[14] = 0x101;
        }
        if (round_number % 79 == 0) {
            both(0x40424, 4, 0x600000);
            both(0x40428, 4, 0x600200);
            both(0x40430, 4, 0x60020f);
            both(0x40420, 2, 17);
        }
        if (round_number % 83 == 0) {
            a->periodic_time = b->periodic_time = !!(rng & 4096);
        }
        if (round_number % 89 == 0)
            both(0x4010c, 4, (rng & 8192) ? 0x04000000 : 0);
        if (round_number % 97 == 0)
            both(0x4090c, 4, (rng & 16384) ? 8 : 0);
        for (i = 0; i < n; ++i) {
            if (!machine_step(a, 1))
                fprintf(stderr, "Reference stop PC=%08x last=%08x op=%08x error=%s\n", a->cpu.r[15],
                        a->cpu.last_pc, a->cpu.last_opcode, a->cpu.error ? a->cpu.error : "");
            CHECK(!a->cpu.halted);
        }
        CHECK(machine_run(b, n, 1) == n);
        compare();
    }
    /* Strict faults stop on the same instruction and preserve diagnostics. */
    a->cpu.cpsr = b->cpu.cpsr = 0xf3;
    a->cpu.r[15] = b->cpu.r[15] = 0x100;
    a->cpu.r[0] = b->cpu.r[0] = 0xdead0000;
    CHECK(!machine_step(a, 0));
    CHECK(machine_run(b, 64, 0) == 0);
    compare();
    cx805_boundaries();
    cx805_timer_pulse();
    machine_destroy(a);
    machine_destroy(b);
    free(a);
    free(b);
    printf("Machine batch differential: %u checks passed.\n", checks);
    return 0;
}
