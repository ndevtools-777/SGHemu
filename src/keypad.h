#ifndef SGH_KEYPAD_H
#define SGH_KEYPAD_H
#include "intc.h"

enum { KEYPAD_IRQ = 1u << 24 };

typedef struct {
    Bus *bus;
    Intc *intc;
    uint32_t base, held, mapped;
    uint16_t control, drive, returns;
    uint8_t row[32], column[32], rows, columns;
    uint64_t reads, writes;
    int attached;
} Keypad;

int keypad_attach(Keypad *d, Bus *bus, uint32_t base, unsigned rows, unsigned columns);
void keypad_reset(Keypad *d);
int keypad_map(Keypad *d, unsigned key, unsigned row, unsigned column);
void keypad_connect_irq(Keypad *d, Intc *intc);
void keypad_key(Keypad *d, unsigned key, int down);
#endif
