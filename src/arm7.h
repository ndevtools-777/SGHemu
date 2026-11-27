#ifndef SGH_ARM7_H
#define SGH_ARM7_H
#include "bus.h"
#define ARM_N 0x80000000u
#define ARM_Z 0x40000000u
#define ARM_C 0x20000000u
#define ARM_V 0x10000000u
#define ARM_I 0x80u
#define ARM_F 0x40u
#define ARM_T 0x20u
typedef struct Arm7Cache Arm7Cache;

typedef struct {
    uint32_t r[16], cpsr;
    uint32_t bank[6][2], spsr[6], user_hi[5], fiq_hi[5];
    uint64_t instructions, cycles;
    uint64_t clock_cycles; /* ARM7 core cycles, excluding external memory waits. */
    uint32_t last_pc, last_opcode;
    int halted, irq, fiq;
    const char *error;
    Bus *bus;
    Arm7Cache *cache; /* borrowed; no guest state or address-dependent values */
    int vector_undefined;
    uint64_t undefined_count, prefetch_abort_count, data_abort_count;
    uint32_t fault_vector, fault_pc, fault_opcode, fault_address;
} Arm7;

Arm7Cache *arm7_cache_create(void);
void arm7_cache_destroy(Arm7Cache *cache);
void arm7_reset(Arm7 *cpu, Bus *bus, uint32_t pc);
int arm7_step(Arm7 *cpu);
int arm7_set_cpsr(Arm7 *cpu, uint32_t value);
int arm7_fault_status(const Arm7 *cpu, char *text, size_t size);
#endif
