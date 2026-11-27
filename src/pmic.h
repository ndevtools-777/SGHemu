#ifndef SGH_PMIC_H
#define SGH_PMIC_H
#include "bus.h"

enum {
    PMIC_POWER_CONTROL = 0x03,
    PMIC_POWER_HOLD = 1u << 0,
    PMIC_POWER_STATUS = 0x08,
    PMIC_POWER_KEY = 1u << 0,
    PMIC_POWER_ALARM = 1u << 1
};

typedef struct {
    Bus *bus;
    uint32_t base;
    uint16_t lines[2];
    uint8_t regs[256], reset_regs[256];
    unsigned address, active, bits, byte, stage, index, reading, ack, selected;
    uint64_t transfers;
    int attached, key_supply;
} Pmic;

int pmic_attach(Pmic *p, Bus *bus, uint32_t base, unsigned address);
void pmic_reset(Pmic *p, unsigned power_status, int key_supply);

static inline int pmic_holds_power(const Pmic *p)
{
    return p->key_supply || (p->regs[PMIC_POWER_CONTROL] & PMIC_POWER_HOLD);
}
#endif
