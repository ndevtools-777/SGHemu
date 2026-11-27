#include "keypad.h"
#include <string.h>

static void update_returns(Keypad *d)
{
    const unsigned inactive = (d->drive >> 8) | d->control;
    uint16_t returns = 0xffff;
    unsigned i;
    for (i = 0; i < 32; ++i)
        if ((d->held & (1u << i)) && !(inactive & (1u << d->row[i])))
            returns &= (uint16_t)~(1u << d->column[i]);
    /* A pulled-low return line latches Keydetect (INTC1 bit 24). */
    if (d->intc && (d->returns & ~returns))
        d->intc->pending |= KEYPAD_IRQ;
    d->returns = returns;
}

static int access(Keypad *d, uint32_t offset, unsigned width, int write, uint32_t value)
{
    if ((width != 1 && width != 2) || (offset & 1u) + width > 2 || offset >= 6) {
        bus_fault_access(d->bus, write, d->base + offset, width, value, "invalid keypad register access");
        return 0;
    }
    return 1;
}

static uint32_t read_keypad(void *ctx, uint32_t offset, unsigned width)
{
    Keypad *d = ctx;
    uint16_t value;
    if (!access(d, offset, width, 0, 0))
        return 0;
    value = offset < 2 ? d->control : offset < 4 ? d->drive : d->returns;
    ++d->reads;
    return (value >> (8 * (offset & 1))) & (width == 1 ? 255u : 65535u);
}

static void write_keypad(void *ctx, uint32_t offset, unsigned width, uint32_t value)
{
    Keypad *d = ctx;
    unsigned shift = 8 * (offset & 1);
    if (!access(d, offset, width, 1, value))
        return;
    if (offset >= 4) {
        bus_fault_access(d->bus, 1, d->base + offset, width, value, "read-only keypad columns");
        return;
    }
    if (offset < 2) {
        unsigned mask = (width == 1 ? 0xffu : 0xffffu) << shift;
        d->control = (uint16_t)((d->control & ~mask) | ((value << shift) & mask));
    } else if (width == 2) {
        /* Like the GPIO data ports, strobe data is in the high byte. */
        unsigned mask = (value & 0xffu) << 8;
        d->drive = (uint16_t)((d->drive & ~mask) | (value & mask));
    } else if (offset == 3) {
        d->drive = (uint16_t)((value << 8) | (d->drive & 0xffu));
    }
    update_returns(d);
    ++d->writes;
}

int keypad_attach(Keypad *d, Bus *bus, uint32_t base, unsigned rows, unsigned columns)
{
    if (d->attached || (base & 1) || !rows || rows > 8 || !columns || columns > 5)
        return 0;
    memset(d, 0, sizeof(*d));
    d->bus = bus;
    d->base = base;
    d->rows = (uint8_t)rows;
    d->columns = (uint8_t)columns;
    keypad_reset(d);
    if (!bus_device(bus, base, 6, read_keypad, write_keypad, d))
        return 0;
    d->attached = 1;
    return 1;
}

void keypad_reset(Keypad *d)
{
    d->held = 0;
    d->control = 0;
    d->drive = 0x00ff;
    d->returns = 0xffff;
}

int keypad_map(Keypad *d, unsigned key, unsigned row, unsigned column)
{
    if (!d->attached || key >= 32 || row >= d->rows || column >= d->columns || (d->mapped & (1u << key)))
        return 0;
    /* Host aliases may share a switch. Each held bit remains independent. */
    d->mapped |= 1u << key;
    d->row[key] = (uint8_t)row;
    d->column[key] = (uint8_t)column;
    return 1;
}

void keypad_key(Keypad *d, unsigned key, int down)
{
    if (key >= 32 || !(d->mapped & (1u << key)))
        return;
    uint32_t before = d->held;
    if (down)
        d->held |= 1u << key;
    else
        d->held &= ~(1u << key);
    if (d->held != before)
        update_returns(d);
}

void keypad_connect_irq(Keypad *d, Intc *intc)
{
    if (d->attached && intc->attached)
        d->intc = intc;
}
