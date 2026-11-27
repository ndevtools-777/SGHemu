#include "machine.h"
#include "nvram.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned checks;
enum { KEY_TABLE_SELECTOR = 0x004065f8, NO_SIM_STANDBY_TABLE = 0x013f0394 };
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        ++checks;                                                                                            \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "C100 keypad line %d: %s\n", __LINE__, #x);                                      \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)

static int press(Machine *m, unsigned key, unsigned action)
{
    unsigned scan = (m->keypad.row[key] << 5) + m->keypad.column[key] + 1;
    unsigned seen_phases = 0;
    int seen_action = 0;
    for (unsigned phase = 0; phase < 2; ++phase) {
        machine_key(m, key, phase == 0);
        for (unsigned i = 0; i < 2000000; ++i) {
            /* XEDK1 kid_KbdParser, then ker_event. Ignore unrelated events. */
            if (m->cpu.r[15] == 0x011390c8 && m->cpu.r[1] == scan && m->cpu.r[0] == phase)
                seen_phases |= 1u << phase;
            if (m->cpu.r[15] == 0x0112d398 && m->cpu.r[0] == action)
                seen_action = 1;
            if (!machine_step(m, 1))
                return 0;
        }
    }
    if (seen_phases == 3 && seen_action)
        return 1;
    fprintf(stderr, "Host key %u: scan %02x phases=%u, expected action %02x seen=%d\n", key, scan,
            seen_phases, action, seen_action);
    return 0;
}

int main(int argc, char **argv)
{
    static const unsigned expected[22] = {0x47, 0x24, 0x48, 0x44, 0x47, 0x45, 0x27, 0x25, 0x28, '1', '2',
                                          '3',  '4',  '5',  '6',  '7',  '8',  '9',  '*',  '0',  '#', 0x26};
    Machine *m = calloc(1, sizeof(*m));
    unsigned key;
    uint32_t table;
    if (argc != 2 || !m) {
        fprintf(stderr, "Usage: c100_keypad_test COMPLETED-XEDK1.snv\n");
        free(m);
        return 2;
    }
    CHECK(machine_load(m, "models/c100/c100xedk1-bringup.cfg", NULL, NULL));
    CHECK(nvram_load(m, argv[1])); /* Read-only fixture: never changes the user's save. */
    m->sim.present = 0;            /* Keep the no-SIM UI fixture used for these input assertions. */
    /* XEDK1 can select the no-SIM table transiently during boot, before
       keypad input is serviced. Give the startup UI time to settle. */
    CHECK(machine_run(m, 200000000, 1) == 200000000);
    table = bus_read(&m->bus, KEY_TABLE_SELECTOR, 4);
    CHECK(table == NO_SIM_STANDBY_TABLE);
    for (key = 0; key < 22; ++key) {
        unsigned raw = ((unsigned)m->keypad.row[key] << 5) + m->keypad.column[key] + 1, entry;
        CHECK(m->keypad.mapped & (1u << key));
        for (entry = 0; entry < 32; ++entry) {
            unsigned code = bus_read(&m->bus, table + entry * 8, 1);
            if (!code || code == raw)
                break;
        }
        CHECK(entry < 32 && bus_read(&m->bus, table + entry * 8, 1) == raw);
        CHECK(bus_read(&m->bus, table + entry * 8 + 5, 1) == expected[key]);
    }
    CHECK(press(m, 9, '1'));
    CHECK(press(m, 10, '2'));  /* Dial 12. */
    CHECK(press(m, 21, 0x26)); /* Real kid_HandleClear: delete the last digit. */
    CHECK(lcd_save_bmp(&m->lcd, "build/keypad-clear.bmp"));
    CHECK(press(m, 8, 0x28)); /* End leaves the dial input. */
    CHECK(lcd_save_bmp(&m->lcd, "build/keypad-end.bmp"));
    CHECK(press(m, 0, 0x47));
    CHECK(lcd_save_bmp(&m->lcd, "build/keypad-soft-left.bmp"));
    CHECK(press(m, 2, 0x48));
    CHECK(lcd_save_bmp(&m->lcd, "build/keypad-soft-right.bmp"));
    printf("C100 guest keypad: mapping and live event checks passed (%u checks).\n", checks);
    machine_destroy(m);
    free(m);
    return 0;
}
