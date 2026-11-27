#include "machine.h"
#include <stdio.h>
#include <stdlib.h>
static unsigned checks;
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        ++checks;                                                                                            \
        if (!(x)) {                                                                                          \
            fprintf(stderr, "Keypad line %d: %s\n", __LINE__, #x);                                           \
            exit(1);                                                                                         \
        }                                                                                                    \
    } while (0)

int main(void)
{
    Machine m = {0};
    unsigned row;
    uint16_t held;
    CHECK(intc_attach(&m.intc, &m.bus, 0x40100));
    CHECK(keypad_attach(&m.keypad, &m.bus, 0x40800, 8, 4));
    keypad_connect_irq(&m.keypad, &m.intc);
    CHECK(!keypad_attach(&m.keypad, &m.bus, 0x40800, 8, 4));
    CHECK(keypad_map(&m.keypad, 9, 4, 0));
    CHECK(keypad_map(&m.keypad, 10, 4, 1));
    CHECK(keypad_map(&m.keypad, 0, 0, 1));
    CHECK(keypad_map(&m.keypad, 8, 7, 2));
    CHECK(!keypad_map(&m.keypad, 9, 5, 0));
    CHECK(keypad_map(&m.keypad, 11, 4, 0));
    CHECK(!keypad_map(&m.keypad, 32, 0, 0));
    CHECK(!keypad_map(&m.keypad, 11, 8, 0));
    CHECK(!keypad_map(&m.keypad, 11, 0, 4));
    CHECK(bus_read(&m.bus, 0x40800, 2) == 0);
    CHECK(bus_read(&m.bus, 0x40802, 2) == 0x00ff);
    CHECK(bus_read(&m.bus, 0x40804, 2) == 0xffff);
    bus_write(&m.bus, 0x40802, 2, 0xffff);
    machine_key(&m, 9, 1);
    CHECK(m.keys == (1u << 9));
    CHECK(bus_read(&m.bus, 0x40804, 1) == 255); /* No driven row means no key. */
    CHECK(!m.intc.pending);
    bus_write(&m.bus, 0x40802, 2, 0x0010); /* Change only row 4. */
    CHECK(bus_read(&m.bus, 0x40802, 2) == 0xefff);
    CHECK(bus_read(&m.bus, 0x40804, 1) == 254);
    CHECK(bus_read(&m.bus, 0x40100, 4) == KEYPAD_IRQ);
    CHECK(!bus_read(&m.bus, 0x40118, 4)); /* Pending also latches while masked. */
    bus_write(&m.bus, 0x40108, 4, KEYPAD_IRQ);
    CHECK(bus_read(&m.bus, 0x40118, 4) == KEYPAD_IRQ);
    bus_write(&m.bus, 0x40100, 4, KEYPAD_IRQ);
    machine_key(&m, 11, 1);
    machine_key(&m, 9, 0);
    CHECK(bus_read(&m.bus, 0x40804, 1) == 254); /* Releasing one alias leaves the switch held. */
    CHECK(!m.intc.pending);
    machine_key(&m, 11, 0);
    CHECK(bus_read(&m.bus, 0x40804, 1) == 255);
    CHECK(!m.intc.pending);
    machine_key(&m, 9, 1);
    CHECK(m.intc.pending == KEYPAD_IRQ);
    bus_write(&m.bus, 0x40800, 1, 1u << 4);
    CHECK(bus_read(&m.bus, 0x40804, 1) == 255); /* The strobe is an input. */
    bus_write(&m.bus, 0x40800, 1, 0);
    for (row = 0; row < 8; ++row) {
        bus_write(&m.bus, 0x40802, 2, ((255u ^ (1u << row)) << 8) | 255u);
        CHECK(bus_read(&m.bus, 0x40804, 1) == (row == 4 ? 254u : 255u));
    }
    machine_key(&m, 10, 1);
    machine_key(&m, 0, 1);
    machine_key(&m, 8, 1);
    bus_write(&m.bus, 0x40802, 2, 0xefff);
    CHECK(bus_read(&m.bus, 0x40804, 1) == 252);
    bus_write(&m.bus, 0x40802, 2, 0xfeff);
    CHECK(bus_read(&m.bus, 0x40804, 1) == 253);
    bus_write(&m.bus, 0x40802, 2, 0x7fff);
    CHECK(bus_read(&m.bus, 0x40804, 1) == 251);
    bus_write(&m.bus, 0x40802, 2, 0xffff);
    CHECK(bus_read(&m.bus, 0x40804, 1) == 255);
    bus_write(&m.bus, 0x40803, 1, 0x6f);
    CHECK(bus_read(&m.bus, 0x40802, 2) == 0x6fff);
    CHECK(bus_read(&m.bus, 0x40804, 2) == 0xfff8); /* Multiple selected rows, wired active-low columns. */
    CHECK(bus_read(&m.bus, 0x40805, 1) == 255);
    held = m.keypad.drive;
    bus_write(&m.bus, 0x40804, 2, 0);
    CHECK(m.bus.fault && m.keypad.drive == held);
    m.bus.fault = 0;
    (void)bus_read(&m.bus, 0x40803, 2);
    CHECK(m.bus.fault);
    m.bus.fault = 0;
    (void)bus_read(&m.bus, 0x40800, 4);
    CHECK(m.bus.fault);
    m.bus.fault = 0;
    bus_write(&m.bus, 0x40800, 2, 0x1234);
    CHECK(bus_read(&m.bus, 0x40800, 2) == 0x1234);
    for (row = 0; row < SGH_MAX_KEYS; ++row)
        machine_key(&m, row, 0);
    CHECK(!m.keypad.held && !m.keys);
    CHECK(bus_read(&m.bus, 0x40804, 1) == 255);
    machine_key(&m, 31, 1);
    CHECK(!m.keypad.held && !m.keys);
    bus_destroy(&m.bus);
    {
        Keypad invalid = {0};
        Bus b = {0};
        CHECK(!keypad_attach(&invalid, &b, 0x40800, 9, 4));
        CHECK(!keypad_attach(&invalid, &b, 0x40800, 8, 6));
        CHECK(!keypad_attach(&invalid, &b, 0x40801, 8, 4));
        CHECK(!keypad_attach(&invalid, &b, 0x40800, 0, 4));
    }
    printf("Keypad matrix: %u checks passed.\n", checks);
    return 0;
}
