#include "port.h"
#include "nvram.h"
#include "firmware.h"
#include <stdlib.h>
#include <string.h>
static unsigned checks;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        ++checks;                                                                                            \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "NVM test line %d: %s\n", __LINE__, #x);                                         \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)

static void config(const char *extra)
{
    FILE *f = fopen("build/nv-fixture.cfg", "w");
    CHECK(f != NULL);
    fputs(
        "rom=0x1000000,nv-fixture.bin,0x10000\nflash=0x1000000,0xffff,0xffff\nreset=0x1000000\naudio=silent\n",
        f);
    fputs(extra, f);
    CHECK(fclose(f) == 0);
}

static void standby(Machine *m)
{
    NvramStore store = {0};
    const RtcDate date = {1996, 1, 1, 0, 0, 56};
    FILE *uart = tmpfile();
    CHECK(uart);
    config("nvram=0x1000020,0xffe0\nram=0x400000,0x100\nlcd_s6b33=0xc00000,0xc80000,0\n"
           "intc=0x40100\nclock=0x40b00,3900000\nclock_control=0x40902,0x600\n"
           "pmic=0x40300,0x90\npmic_register=0x11,0x5a\nc100_power_key_ms=2\nrtc=0x40080,1996\n"
           "uart=0x40200,debug\nuart=0x41200,sds\nsim=0x40a80,0x41300,0x40520\n");
    CHECK(machine_load(m, "build/nv-fixture.cfg", NULL, NULL));
    CHECK(nvram_store_open(&store, m, "build/nv-standby"));
    CHECK(machine_start_standby(m) && machine_powered_off(m) && !m->cpu.instructions);
    CHECK(!machine_advance_off(m, 1000000) && !m->lcd.powered && !machine_step(m, 0));
    machine_key(m, SGH_KEY_POWER, 1);
    CHECK(!machine_powered_off(m) && m->lcd.powered && !m->lcd.enabled && !m->pixels[0]);
    CHECK(m->c100.wake_source == C100_WAKE_KEY && m->clock.nanoseconds == 1000000);
    CHECK(machine_run(m, 4096, 0) == 4096 && m->c100.release_ns == UINT64_MAX);
    m->cpu.halted = 1; /* The PMU still sees the release if the CPU stops before HOLD. */
    machine_key(m, SGH_KEY_POWER, 0);
    CHECK(machine_powered_off(m) && !m->lcd.powered && !m->pixels[0]);
    /* Even a tap between two CPU steps briefly applies power. */
    machine_key(m, SGH_KEY_POWER, 1);
    CHECK(!machine_powered_off(m) && !m->cpu.halted && !m->pixels[0]);
    machine_key(m, SGH_KEY_POWER, 0);
    CHECK(machine_powered_off(m) && !m->pixels[0] && m->cpu.instructions == 4096);

    machine_key(m, SGH_KEY_POWER, 1);
    CHECK(!machine_powered_off(m));
    m->pmic.regs[PMIC_POWER_CONTROL] = PMIC_POWER_HOLD;
    machine_key(m, SGH_KEY_POWER, 0);
    CHECK(machine_step(m, 0));

    m->nvram_data[0] = 0x42;
    CHECK(nvram_store_flush(&store, m));
    m->nvram_data[0] = 0x33; /* An interrupted import modified live flash. */
    CHECK(!nvram_store_restore_interrupted(&store, m) && m->nvram_data[0] == 0x33);
    m->pmic.regs[0x11] = 0xaa;
    m->sim.present = 1;
    strcpy(m->sim.card.spn, "Saved SIM");
    machine_set_battery(m, 73, 1);

    Bus *bus = &m->bus;
    m->debug_uart.tx_log = uart;
    bus_write(bus, 0x40d08, 2, 0x0400); /* C5 routes DEBUG TX. */
    bus_write(bus, 0x40202, 2, 3);
    bus_write(bus, 0x40200, 2, 1);
    bus_write(bus, 0x40204, 2, 'A');
    CHECK(machine_run(m, 4096, 0) == 4096 && m->debug_uart.pin_transmits == 1);
    CHECK(rtc_set_host_time(&m->rtc, &date));
    bus_write(bus, 0x400a0, 2, 0xface);
    bus_write(bus, 0x40092, 2, 1);
    for (uint32_t reg = 0x40094; reg <= 0x4009a; reg += 2)
        bus_write(bus, reg, 2, 0);
    const uint64_t alarm = m->rtc.alarm_due;

    bus_write(bus, 0x400000, 4, 0x12345678);
    bus_write(bus, 0x400004, 4, 0xe1a00000); /* NOP for the two-instruction batch. */
    bus_write(bus, 0x400b0, 2, 0x001e);
    bus_write(bus, 0x400b2, 2, 1);
    bus_write(bus, 0x40b04, 2, 0x0008); /* A normal PLL write does not request reset. */
    CHECK(machine_step(m, 0) && m->c100.reset_count == 3);
    bus_write(bus, 0x40204, 2, 'B');
    CHECK(m->debug_uart.tx_due != UINT64_MAX);
    for (unsigned batch = 1; batch <= 2; ++batch) {
        CHECK(bus_read(bus, 0x4091e, 2) == 0x0047);
        bus_write(bus, 0x4091e, 2, 0x4567);
        CHECK(bus_read(bus, 0x4091e, 2) == 0x4567);

        /* STRH to External Control in a single step, then STRB to its high byte in a batch. */
        bus_write(bus, 0x400008, 4, batch == 1 ? 0xe1c010b0 : 0xe5c01000);
        m->cpu.r[0] = batch == 1 ? 0x40b04 : 0x40b05;
        m->cpu.r[1] = batch == 1 ? 0x8000 : 0x80;
        m->cpu.r[15] = batch == 1 ? 0x400008 : 0x400004;
        const uint64_t instructions = m->cpu.instructions, now = m->clock.nanoseconds;
        CHECK(machine_run(m, batch, 0) == batch && m->c100.reset_count == 3 + batch);
        CHECK(m->cpu.instructions == instructions + batch && m->clock.nanoseconds > now);
        CHECK(!machine_powered_off(m) && !m->cpu.halted && m->cpu.r[15] == m->reset && !bus->faults);
        CHECK(!bus_read(bus, 0x40b04, 2) && bus_read(bus, 0x4091e, 2) == 0x0047);
        CHECK(!m->lcd.enabled && m->lcd.backlight == LCD_BACKLIGHT_OFF && !m->pixels[0]);
        CHECK(m->pmic.regs[0x11] == 0xaa && m->pmic.regs[PMIC_POWER_CONTROL] == PMIC_POWER_HOLD);
        CHECK(m->pmic.regs[PMIC_POWER_STATUS] == PMIC_POWER_KEY && m->c100.wake_source == C100_WAKE_KEY);
        CHECK(m->battery.charging && m->battery.percent == 73);
        CHECK(m->rtc.alarm_due == alarm && bus_read(bus, 0x400b0, 4) == 0x0001001e);
        CHECK(m->sim.present && !strcmp(m->sim.card.spn, "Saved SIM"));
        CHECK(bus_read(bus, 0x400000, 4) == 0x12345678 && bus_read(bus, m->nvram_base, 1) == 0x33);
        CHECK(m->debug_uart.tx_log == uart && m->debug_uart.pin_transmits == 1 &&
              m->debug_uart.tx_due == UINT64_MAX);
    }
    CHECK(nvram_store_restore_interrupted(&store, m) && m->nvram_data[0] == 0x42);
    CHECK(machine_step(m, 0));
    m->nvram_data[0] = 0x33;
    CHECK(!nvram_store_restore_interrupted(&store, m) && m->nvram_data[0] == 0x33);

    bus_write(bus, 0x40d08, 2, 0x0400);
    bus_write(bus, 0x40202, 2, 3);
    bus_write(bus, 0x40200, 2, 1);
    bus_write(bus, 0x40204, 2, 'W');
    CHECK(machine_run(m, 32, 0) == 32 && m->debug_uart.pin_transmits == 2);
    bus_write(bus, 0x40204, 2, 'B');
    bus_write(bus, 0x41202, 2, 3);
    bus_write(bus, 0x41200, 2, 1);
    bus_write(bus, 0x41204, 2, 'S');
    CHECK(m->debug_uart.tx_due != UINT64_MAX && m->sds_uart.tx_due != UINT64_MAX);
    m->pmic.regs[PMIC_POWER_CONTROL] = 0;
    machine_key(m, SGH_KEY_POWER, 1); /* The shutdown press is still held. */
    CHECK(machine_run(m, 64, 0) == 0 && machine_powered_off(m));
    const uint64_t stopped = m->cpu.instructions, off = m->clock.nanoseconds;
    CHECK(!m->lcd.powered && !m->pixels[0]);
    CHECK(m->debug_uart.tx_due == UINT64_MAX && m->sds_uart.tx_due == UINT64_MAX);
    CHECK(nvram_store_restore_interrupted(&store, m) && m->nvram_data[0] == 0x42);
    machine_set_battery(m, 73, 1); /* A cable already in place is not an insertion. */
    machine_key(m, SGH_KEY_POWER, 1);
    machine_key(m, 0, 1);
    machine_key(m, 0, 0);
    CHECK(!machine_advance_off(m, 1000000000) && machine_powered_off(m) && !machine_step(m, 0));
    CHECK(m->rtc.date.second == 57 && m->cpu.instructions == stopped && m->clock.nanoseconds == off);
    CHECK(!m->intc.pending || !(m->intc.pending & RTC_ALARM_IRQ));

    machine_key(m, SGH_KEY_POWER, 0);
    machine_key(m, SGH_KEY_POWER, 1);
    CHECK(!machine_powered_off(m) && m->c100.wake_source == C100_WAKE_KEY);
    CHECK(m->pmic.regs[PMIC_POWER_STATUS] == PMIC_POWER_KEY && m->pmic.regs[0x11] == 0x5a);
    CHECK(!(bus_read(bus, 0x40804, 2) & 8));
    CHECK(m->cpu.r[15] == m->reset && !m->cpu.halted && m->cpu.instructions == stopped);
    CHECK(m->lcd.powered && !m->lcd.enabled && m->lcd.backlight == LCD_BACKLIGHT_OFF && !m->pixels[0]);
    CHECK(!bus_read(bus, 0x400000, 4));
    CHECK(bus_read(bus, 0x400a0, 2) == 0xface && m->sim.present && !strcmp(m->sim.card.spn, "Saved SIM"));
    CHECK(m->debug_uart.tx_log == uart && m->debug_uart.pin_transmits == 2 && m->sds_uart.transmits == 0);

    machine_key(m, SGH_KEY_POWER, 0);
    CHECK(machine_powered_off(m));
    machine_set_battery(m, 62, 0);
    CHECK(!machine_advance_off(m, 0));
    machine_set_battery(m, 62, 1);
    CHECK(machine_advance_off(m, 0) && m->c100.wake_source == C100_WAKE_CHARGER);
    CHECK(!m->pmic.regs[PMIC_POWER_STATUS] && m->battery.charging && m->battery.percent == 62);
    CHECK(machine_run(m, 4096, 0) < 4096 && machine_powered_off(m));
    CHECK(!machine_advance_off(m, alarm - m->c100.off_ns - 1));
    CHECK(machine_advance_off(m, 1000000000) && m->clock.nanoseconds == alarm);
    CHECK(m->c100.wake_source == C100_WAKE_RTC && m->c100.reset_count == 8);
    CHECK(m->pmic.regs[PMIC_POWER_STATUS] == PMIC_POWER_ALARM && m->rtc.alarm_events == 1 && m->rtc.alarm_match);
    bus_write(bus, 0x40092, 2, 2); /* Firmware can acknowledge the alarm before setting HOLD. */
    CHECK(!m->rtc.alarm_match && machine_run(m, 32, 0) == 32);
    bus_write(bus, 0x40d08, 2, 0x0400);
    bus_write(bus, 0x40202, 2, 3);
    bus_write(bus, 0x40200, 2, 1);
    bus_write(bus, 0x40204, 2, 'C');
    CHECK(machine_run(m, 32, 0) == 32);
    rewind(uart);
    CHECK(fgetc(uart) == 'A' && fgetc(uart) == 'W' && fgetc(uart) == 'C' && fgetc(uart) == EOF);
    m->debug_uart.tx_log = NULL;
    CHECK(fclose(uart) == 0);

    m->nvram_data[1] = 0x12;
    CHECK(nvram_store_flush(&store, m));
    nvram_store_release(&store);
    CHECK(machine_load(m, "build/nv-fixture.cfg", NULL, NULL));
    CHECK(nvram_store_open(&store, m, "build/nv-standby"));
    CHECK(m->nvram_data[0] == 0x42 && m->nvram_data[1] == 0x12);
    char saved[sizeof(store.path)];
    strcpy(saved, store.path);
    nvram_store_release(&store);
    CHECK(plat_remove(saved));
}

int main(void)
{
    Machine m = {0};
    FILE *f;
    uint8_t code[32] = {0xfe, 0xff, 0xff, 0xea};
    char before[65], after[65];
    f = fopen("build/nv-fixture.bin", "wb");
    CHECK(f != NULL);
    CHECK(fwrite(code, 1, sizeof(code), f) == sizeof(code));
    CHECK(fclose(f) == 0);
    CHECK(firmware_sha256("build/nv-fixture.bin", before));
    config("nvram=0x1000020,0xffe0\n");
    CHECK(machine_load(&m, "build/nv-fixture.cfg", NULL, NULL));
    CHECK(m.nvram_data && m.nvram_size == 0xffe0 && m.nvram_data[0] == 255);
    m.nvram_data[0] = 0x12;
    m.nvram_data[m.nvram_size - 1] = 0x34;
    CHECK(nvram_save(&m, "build/nv-fixture.snv"));
    memset(m.nvram_data, 0x56, m.nvram_size);
    CHECK(nvram_load(&m, "build/nv-fixture.snv"));
    CHECK(m.nvram_data[0] == 0x12 && m.nvram_data[m.nvram_size - 1] == 0x34 && m.nvram_data[1] == 255);
    CHECK(!nvram_save(&m, "build/nv-fixture.bin"));
    (void)plat_remove("build/nv-rom-alias.bin");
    CHECK(plat_hard_link("build/nv-fixture.bin", "build/nv-rom-alias.bin"));
    CHECK(!nvram_save(&m, "build/nv-rom-alias.bin"));
    CHECK(plat_remove("build/nv-rom-alias.bin"));
    CHECK(firmware_sha256("build/nv-fixture.bin", after) && !strcmp(before, after));
    CHECK(machine_step(&m, 0));
    CHECK(!nvram_load(&m, "build/nv-fixture.snv"));
    CHECK(machine_load(&m, "build/nv-fixture.cfg", NULL, NULL));
    f = fopen("build/nv-fixture.snv", "r+b");
    CHECK(f != NULL);
    CHECK(fseek(f, 344, SEEK_SET) == 0 && fputc(0x99, f) != EOF);
    CHECK(fclose(f) == 0);
    CHECK(!nvram_load(&m, "build/nv-fixture.snv"));
    CHECK(m.nvram_data[0] == 255 && m.nvram_data[m.nvram_size - 1] == 255);
    CHECK(nvram_save(&m, "build/nv-fixture.snv")); /* Atomic replacement of an existing snapshot. */
    f = fopen("build/nv-fixture.snv", "ab");
    CHECK(f != NULL);
    CHECK(fputc(1, f) != EOF);
    CHECK(fclose(f) == 0);
    CHECK(!nvram_load(&m, "build/nv-fixture.snv"));
    CHECK(m.nvram_data[0] == 255);
    CHECK(nvram_save(&m, "build/nv-fixture.snv"));
    code[4] = 1;
    f = fopen("build/nv-fixture.bin", "wb");
    CHECK(f != NULL);
    CHECK(fwrite(code, 1, sizeof(code), f) == sizeof(code));
    CHECK(fclose(f) == 0);
    CHECK(machine_load(&m, "build/nv-fixture.cfg", NULL, NULL));
    CHECK(!nvram_load(&m, "build/nv-fixture.snv"));
    CHECK(m.nvram_data[0] == 255);
    f = fopen("build/nv-fixture.snv", "wb");
    CHECK(f != NULL);
    CHECK(fputs("SGHNVM1", f) >= 0);
    CHECK(fclose(f) == 0);
    CHECK(!nvram_load(&m, "build/nv-fixture.snv"));
    f = fopen("build/nv-fixture.snv.tmp", "wb");
    CHECK(f != NULL);
    CHECK(fputs("leave untouched", f) >= 0);
    CHECK(fclose(f) == 0);
    CHECK(!nvram_save(&m, "build/nv-fixture.snv"));
    CHECK(plat_remove("build/nv-fixture.snv.tmp"));
    config("nvram=0x100001e,0xffe2\n");
    CHECK(!machine_load(&m, "build/nv-fixture.cfg", NULL, NULL));
    config("nvram=0x1000020,0x10000\n");
    CHECK(!machine_load(&m, "build/nv-fixture.cfg", NULL, NULL));
    config("nvram=0x1000020,0xffe0\nnvram=0x1000020,0xffe0\n");
    CHECK(!machine_load(&m, "build/nv-fixture.cfg", NULL, NULL));
    config("");
    CHECK(machine_load(&m, "build/nv-fixture.cfg", NULL, NULL));
    CHECK(!nvram_save(&m, "build/nv-fixture.snv"));
    /* Automatic persistence: real round trips, lock exclusion, failure preservation,
       version isolation and incomplete first-boot provisioning. */
    {
        NvramStore store = {0}, second = {0};
        char stored[1024], changed[1024], tmp[1100];
        uint64_t instructions;
        config("nvram=0x1000020,0xffe0\n");
        CHECK(machine_load(&m, "build/nv-fixture.cfg", NULL, NULL));
        CHECK(nvram_store_open(&store, &m, "build/nv-auto"));
        strcpy(stored, store.path);
        nvram_store_release(&store);
        (void)plat_remove(stored);
        CHECK(nvram_store_open(&store, &m, "build/nv-auto"));
        CHECK(!store.saved);
        CHECK(!nvram_store_open(&second, &m, "build/nv-auto"));
        CHECK(!second.active && !second.lock);
        m.nvram_data[0] = 0x42;
        CHECK(nvram_store_flush(&store, &m));
        CHECK(store.saved);
        snprintf(tmp, sizeof(tmp), "%s.tmp", store.path);
        f = fopen(tmp, "wb");
        CHECK(f != NULL);
        CHECK(fclose(f) == 0);
        CHECK(nvram_store_flush(&store, &m)); /* Unchanged storage does not rewrite. */
        m.nvram_data[0] = 0x43;
        CHECK(!nvram_store_flush(&store, &m));
        CHECK(plat_remove(tmp));
        nvram_store_release(&store);
        CHECK(machine_load(&m, "build/nv-fixture.cfg", NULL, NULL));
        CHECK(nvram_store_open(&store, &m, "build/nv-auto"));
        CHECK(m.nvram_data[0] == 0x42);
        m.nvram_data[0] = 0x44;
        CHECK(nvram_store_flush(&store, &m));
        nvram_store_release(&store);
        CHECK(machine_load(&m, "build/nv-fixture.cfg", NULL, NULL));
        CHECK(nvram_store_open(&store, &m, "build/nv-auto"));
        CHECK(m.nvram_data[0] == 0x44);
        nvram_store_release(&store);
        f = fopen(stored, "r+b");
        CHECK(f != NULL);
        CHECK(fseek(f, 344, SEEK_SET) == 0 && fputc(0, f) != EOF);
        CHECK(fclose(f) == 0);
        CHECK(machine_load(&m, "build/nv-fixture.cfg", NULL, NULL));
        CHECK(!nvram_store_open(&store, &m, "build/nv-auto"));
        CHECK(!store.active && !store.lock);
        CHECK(m.nvram_data[0] == 255);
        CHECK(plat_remove(stored));
        code[5] ^= 1;
        f = fopen("build/nv-fixture.bin", "wb");
        CHECK(f != NULL);
        CHECK(fwrite(code, 1, sizeof(code), f) == sizeof(code));
        CHECK(fclose(f) == 0);
        CHECK(machine_load(&m, "build/nv-fixture.cfg", NULL, NULL));
        CHECK(nvram_store_open(&store, &m, "build/nv-auto"));
        strcpy(changed, store.path);
        CHECK(strcmp(changed, stored) != 0);
        nvram_store_release(&store);
        (void)plat_remove(changed);
        /* Tiny Thumb factory functions return their documented success values. */
        code[8] = 0;
        code[9] = 0x20;
        code[10] = 0x70;
        code[11] = 0x47;
        code[12] = 1;
        code[13] = 0x20;
        code[14] = 0x70;
        code[15] = 0x47;
        f = fopen("build/nv-fixture.bin", "wb");
        CHECK(f != NULL);
        CHECK(fwrite(code, 1, sizeof(code), f) == sizeof(code));
        CHECK(fclose(f) == 0);
        config("nvram=0x1000020,0xffe0\nnv_provision=0x1000004,0x1000008,0x100000c\n"
               "lcd_s6b33=0xc00000,0xc80000,0\nclock=0x40b00,3900000\npmic=0x40300,0x90\nc100_power_key_ms=2\n");
        CHECK(machine_load(&m, "build/nv-fixture.cfg", NULL, NULL));
        CHECK(nvram_store_open(&store, &m, "build/nv-auto"));
        strcpy(stored, store.path);
        nvram_store_release(&store);
        (void)plat_remove(stored);
        CHECK(nvram_store_open(&store, &m, "build/nv-auto"));
        CHECK(store.provision);
        CHECK(nvram_store_flush(&store, &m));
        CHECK(plat_file_status(stored) == 0);
        m.cpu.cpsr |= ARM_T;
        m.cpu.r[15] = m.nv_trigger;
        m.cpu.r[4] = 0x87654321;
        CHECK(nvram_store_step(&store, &m) == 1 && store.stage == 1);
        CHECK(machine_step(&m, 0));
        CHECK(machine_step(&m, 0));
        CHECK(nvram_store_step(&store, &m) == 1 && store.stage == 2);
        CHECK(nvram_store_flush(&store, &m));
        CHECK(plat_file_status(stored) == 0);
        CHECK(machine_step(&m, 0));
        CHECK(machine_step(&m, 0));
        instructions = m.cpu.instructions;
        m.cpu.clock_cycles = 123456;
        CHECK(nvram_store_step(&store, &m) == 2 && store.stage == 3 && store.saved);
        CHECK(m.cpu.clock_cycles == 123456);
        CHECK(m.cpu.r[4] == 0x87654321 && m.cpu.instructions == instructions && m.cpu.r[15] == m.nv_trigger);
        nvram_store_release(&store);
        CHECK(machine_load(&m, "build/nv-fixture.cfg", NULL, NULL));
        CHECK(nvram_store_open(&store, &m, "build/nv-auto"));
        CHECK(!store.provision);
        store.defaults_boot = 1;
        CHECK(nvram_store_step(&store, &m) == 1 && store.defaults_boot);
        CHECK(irom_add(&m.irom, 0x8077c, "memcpy"));
        m.irom.services[0].calls = 1;
        CHECK(nvram_store_step(&store, &m) == 1 && store.defaults_boot);
        CHECK(irom_add(&m.irom, 0x81fe0, "version"));
        m.irom.services[1].calls = 1;
        CHECK(nvram_store_step(&store, &m) == 2 && !store.defaults_boot);
        CHECK(nvram_store_step(&store, &m) == 1); /* Exactly one settled-default reset. */
        nvram_store_release(&store);
        CHECK(plat_remove(stored));
        /* Neither a reset during the guest formatter nor a failure commits a snapshot. */
        CHECK(machine_load(&m, "build/nv-fixture.cfg", NULL, NULL));
        CHECK(nvram_store_open(&store, &m, "build/nv-auto"));
        m.cpu.cpsr |= ARM_T;
        m.cpu.r[15] = m.nv_trigger;
        CHECK(nvram_store_step(&store, &m) == 1 && store.stage == 1);
        m.nvram_data[0] = 0x37;
        bus_write(&m.bus, 0x40b04, 2, 0x8000);
        CHECK(machine_step(&m, 0) && m.c100.reset_count == 1 && m.cpu.r[15] == m.reset);
        CHECK(nvram_store_step(&store, &m) == 1 && store.stage == 0);
        CHECK(nvram_store_flush(&store, &m) && plat_file_status(stored) == 0);
        m.cpu.cpsr |= ARM_T;
        m.cpu.r[15] = m.nv_trigger;
        CHECK(nvram_store_step(&store, &m) == 1 && store.stage == 1);
        m.cpu.r[15] = m.nv_trigger;
        m.cpu.r[0] = 1;
        CHECK(!nvram_store_step(&store, &m));
        CHECK(nvram_store_flush(&store, &m));
        CHECK(plat_file_status(stored) == 0);
        nvram_store_release(&store);
        config("nvram=0x1000020,0xffe0\nnv_provision=0x1000005,0x1000008,0x100000c\n");
        CHECK(!machine_load(&m, "build/nv-fixture.cfg", NULL, NULL));
        config("nvram=0x1000020,0xffe0\nnv_provision=0x1000004,0x1000040,0x100000c\n");
        CHECK(!machine_load(&m, "build/nv-fixture.cfg", NULL, NULL));
    }
    /* Default placement follows the model config, with a genuine save/reload. */
    {
        NvramStore store = {0};
        char model_save[1024], legacy[1024], *name;
        config("nvram=0x1000020,0xffe0\n");
        CHECK(machine_load(&m, "build/nv-fixture.cfg", NULL, NULL));
        CHECK(nvram_store_open(&store, &m, NULL));
        CHECK(!strncmp(store.path, "build/state/", 12));
        strcpy(model_save, store.path);
        m.nvram_data[123] = 0x42;
        CHECK(nvram_store_flush(&store, &m));
        nvram_store_release(&store);
        CHECK(machine_load(&m, "build/nv-fixture.cfg", NULL, NULL));
        CHECK(nvram_store_open(&store, &m, NULL));
        CHECK(m.nvram_data[123] == 0x42);
        nvram_store_release(&store);
        /* Migration must import the checked legacy snapshot, leaving it intact. */
        name = strrchr(model_save, '/');
        CHECK(name != NULL);
        snprintf(legacy, sizeof(legacy), "state/%s", name + 1);
        CHECK(plat_mkdir("state", 1));
        CHECK(nvram_save(&m, legacy));
        CHECK(plat_remove(model_save));
        CHECK(machine_load(&m, "build/nv-fixture.cfg", NULL, NULL));
        CHECK(nvram_store_open(&store, &m, NULL));
        CHECK(store.saved && !store.provision);
        CHECK(m.nvram_data[123] == 0x42);
        CHECK(plat_file_status(legacy) > 0);
        nvram_store_release(&store);
        CHECK(plat_remove(model_save));
        CHECK(plat_remove(legacy));
    }
    standby(&m);
    machine_destroy(&m);
    printf("NVM snapshot validation: %u checks passed.\n", checks);
    return 0;
}
