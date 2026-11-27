#ifndef SGH_GPIO_H
#define SGH_GPIO_H
#include "intc.h"

/* CX805 eight-pin data port: low byte is the write mask, high byte the value. */
typedef struct {
    Bus *bus;
    uint32_t base;
    uint16_t control, mux, gpio_modes, mux_reset;
    uint8_t output, inputs, driven, output_reset;
    int attached, pins_configured;
    unsigned (*alternate_output)(void *context);
    void (*output_changed)(void *context, unsigned mask, unsigned value, uint64_t ticks);
    void *output_context;
} GpioPort;

int gpio_port_attach(GpioPort *p, Bus *bus, uint32_t base, unsigned reset);
int gpio_port_attach_pins(GpioPort *p, uint32_t control, uint32_t mux, uint16_t mux_reset,
                          uint16_t gpio_modes);
void gpio_port_reset(GpioPort *p);
unsigned gpio_port_output(const GpioPort *p);
int gpio_port_input(GpioPort *p, unsigned pin, int high);
#endif
