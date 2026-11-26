#ifndef SGH_MA5_BRIDGE_H
#define SGH_MA5_BRIDGE_H

#include <stdint.h>

/* Private little-endian stream between Linux and the x86 Windows helper.
   A request's payload is either a UTF-8 DLL path or a complete FIFO packet.
   Generate replies carry two arrays of signed 16-bit samples, left then right. */
#define MA5_BRIDGE_VERSION UINT32_C(0x4d413501)
#define MA5_BRIDGE_PAYLOAD 4096
#define MA5_BRIDGE_FRAMES 512
#define MA5_BRIDGE_ERROR 256

enum {
    MA5_OPEN, MA5_READ, MA5_WRITE, MA5_FIFO, MA5_GENERATE, MA5_PUMP,
    MA5_DETUNE, MA5_WATER, MA5_RESET, MA5_PARAMETER, MA5_MEMORY,
    MA5_CURSOR, MA5_RAM_CHANGED, MA5_REGISTER, MA5_STRESS
};

typedef struct {
    uint32_t version, op, arg, bytes;
} Ma5Request;

typedef struct {
    uint32_t version, value, irq, led, frames, failed, callbacks, adpcm_refreshes;
} Ma5Reply;

#endif
