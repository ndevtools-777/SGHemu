#ifndef SGH_C100_NOR_H
#define SGH_C100_NOR_H
#include <stddef.h>
#include <stdint.h>

enum {
    C100_NOR1 = 0x01000000, C100_NOR2 = 0x02000000, C100_NOR_BANK_SIZE = 0x00800000,
    C100_NOR_ENTRY = 0x38, C100_NOR_LENGTH = 0x40, C100_NOR_MARKER = 0x44, C100_NOR_CODE = 0x54
};

/* First-bank layout seen in XEDK1. This checks bounds and the ARM entry;
   the original IROM's integrity and loader checks have not been recovered. */
static inline const char *cx805_nor_header(void *context, uint32_t (*read)(void *, uint32_t, unsigned),
                                           size_t size, uint32_t *target)
{
    if (size < C100_NOR_CODE + 4)
        return "truncated first bank header";
    if (read(context, C100_NOR_MARKER, 2) != 0xcbcb)
        return "missing CBCB marker at +0x44";
    uint32_t length = read(context, C100_NOR_LENGTH, 4);
    if (length < C100_NOR_CODE + 4 || length > size || length > C100_NOR_BANK_SIZE)
        return "invalid first bank length at +0x40";
    uint32_t entry = read(context, C100_NOR_ENTRY, 4);
    if ((entry >> 24) != 0xea)
        return "expected ARM branch at +0x38";
    uint32_t offset = C100_NOR_ENTRY + 8 + ((entry & 0xffffffu) << 2);
    if (entry & 0x800000u)
        offset -= 0x4000000u; /* Sign-extend the scaled ARM displacement. */
    if (offset < C100_NOR_CODE || offset > length - 4 || read(context, offset, 4) == UINT32_MAX)
        return "entry branch does not reach code in the first image";
    *target = offset;
    return NULL;
}
#endif
