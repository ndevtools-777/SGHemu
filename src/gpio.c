#include "gpio.h"
#include <string.h>

unsigned gpio_port_output(const GpioPort *p)
{
    unsigned pin, output = p->output;
    if (!p->pins_configured)
        return output;
    output &= ~(p->control >> 8); /* A one in OEN tri-states that pin. */
    for (pin = 0; pin < 8; ++pin) {
        if ((p->mux ^ p->gpio_modes) & (3u << (2 * pin)))
            output &= ~(1u << pin);
    }
    return output | (p->alternate_output ? p->alternate_output(p->output_context) : 0);
}

static void notify_pins(GpioPort *p, unsigned mask)
{
    if (p->output_changed)
        p->output_changed(p->output_context, mask, gpio_port_output(p), p->bus->ticks);
}

static uint32_t read_port(void *ctx, uint32_t offset, unsigned width)
{
    GpioPort *p = ctx;
    unsigned sense = (gpio_port_output(p) & ~p->driven) | (p->inputs & p->driven);
    if (offset || width != 2) {
        bus_fault_access(p->bus, 0, p->base + offset, width, 0, "GPIO data port requires x16 access");
        return 0;
    }
    if (!p->pins_configured)
        return sense;
    return ((uint16_t)p->output << 8) | (sense & p->control & 255u);
}

static void write_port(void *ctx, uint32_t offset, unsigned width, uint32_t value)
{
    GpioPort *p = ctx;
    unsigned mask = value & 255u;
    if (offset || width != 2) {
        bus_fault_access(p->bus, 1, p->base + offset, width, value, "GPIO data port requires x16 access");
        return;
    }
    p->output = (uint8_t)((p->output & ~mask) | ((value >> 8) & mask));
    notify_pins(p, mask);
}

static uint32_t read_pin_register(uint16_t value, uint32_t offset, unsigned width)
{
    return width == 2 ? value : (value >> (8 * offset)) & 255u;
}

static void write_pin_register(GpioPort *p, uint16_t *reg, uint32_t offset, unsigned width, uint32_t value)
{
    unsigned shift = 8 * offset;
    unsigned mask = (width == 2 ? 0xffffu : 0xffu) << shift;
    *reg = (uint16_t)((*reg & ~mask) | ((value << shift) & mask));
    notify_pins(p, 255);
}

static uint32_t read_control(void *context, uint32_t offset, unsigned width)
{
    GpioPort *p = context;
    return read_pin_register(p->control, offset, width);
}

static uint32_t read_mux(void *context, uint32_t offset, unsigned width)
{
    GpioPort *p = context;
    return read_pin_register(p->mux, offset, width);
}

static void write_control(void *context, uint32_t offset, unsigned width, uint32_t value)
{
    GpioPort *p = context;
    write_pin_register(p, &p->control, offset, width, value);
}

static void write_mux(void *context, uint32_t offset, unsigned width, uint32_t value)
{
    GpioPort *p = context;
    write_pin_register(p, &p->mux, offset, width, value);
}

int gpio_port_attach_pins(GpioPort *p, uint32_t control, uint32_t mux, uint16_t mux_reset,
                          uint16_t gpio_modes)
{
    if (!p->attached || p->pins_configured || ((control | mux) & 1))
        return 0;
    if (!bus_device(p->bus, control, 2, read_control, write_control, p) ||
        !bus_device(p->bus, mux, 2, read_mux, write_mux, p))
        return 0;
    p->mux_reset = mux_reset;
    p->gpio_modes = gpio_modes;
    p->pins_configured = 1;
    gpio_port_reset(p);
    return 1;
}

int gpio_port_attach(GpioPort *p, Bus *bus, uint32_t base, unsigned reset)
{
    if (p->attached || (base & 1) || reset > 255)
        return 0;
    memset(p, 0, sizeof(*p));
    p->bus = bus;
    p->base = base;
    p->output = p->output_reset = (uint8_t)reset;
    if (!bus_device(bus, base, 2, read_port, write_port, p))
        return 0;
    p->attached = 1;
    return 1;
}

void gpio_port_reset(GpioPort *p)
{
    p->output = p->output_reset;
    p->control = p->pins_configured ? 0xffff : 0;
    p->mux = p->mux_reset;
}

int gpio_port_input(GpioPort *p, unsigned pin, int high)
{
    unsigned mask;
    if (!p || !p->attached || pin >= 8)
        return 0;
    mask = 1u << pin;
    p->driven |= (uint8_t)mask;
    p->inputs = (uint8_t)((p->inputs & ~mask) | (high ? mask : 0));
    return 1;
}
