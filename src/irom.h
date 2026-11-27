#ifndef SGH_IROM_H
#define SGH_IROM_H
#include "arm7.h"
#include "c100_nor.h"
#include "flash.h"
#define IROM_MAX_SERVICES 16

enum { IROM_MEMCPY = 1, IROM_VERSION };

typedef struct {
    Flash *banks[2];
    Region *vectors;
    size_t image_size;
    uint32_t arm_hz;
    uint16_t chip_select[2];
    int enabled, pending;
} C100NorBoot;

typedef struct {
    uint32_t address;
    unsigned kind;
    uint64_t calls;
} IromService;

typedef struct {
    IromService services[IROM_MAX_SERVICES];
    unsigned count;
    uint32_t first_address, last_address;
    uint16_t version;
    uint32_t version_return;
    int version_configured;
    C100NorBoot boot;
} Irom;

int irom_add(Irom *rom, uint32_t address, const char *name);
/* 0: not handled; 1: completed; -1: invalid call, CPU halted.
   The caller must deliver pending CPU exceptions before invoking this hook. */
int irom_step(Irom *rom, Arm7 *cpu);
int irom_cx805_attach(C100NorBoot *boot, Bus *bus, Flash *first, Flash *second, size_t image_size,
                      uint32_t arm_hz);
void irom_cx805_reset(C100NorBoot *boot);
void irom_cx805_set_clock(C100NorBoot *boot, uint32_t arm_hz);
/* Complete the modeled IROM reset sequence before fetching from external NOR. */
int irom_cx805_boot(C100NorBoot *boot, Arm7 *cpu);

static inline int irom_may_call(const Irom *rom, uint32_t pc)
{
    return rom->count && pc >= rom->first_address && pc <= rom->last_address;
}
#endif
