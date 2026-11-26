/* Build for Windows x86 and run under Wine. Keep the actual DLL and its
   worker in this process. stdout is reserved for the PCM/control stream. */
#include "ma5_bridge.h"
#include "ma5_hw.h"
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int16_t left[MA5_BRIDGE_FRAMES], right[MA5_BRIDGE_FRAMES];
    unsigned frames;
} PcmBlock;

static void capture(void *context, int16_t *left, int16_t *right, unsigned frames)
{
    PcmBlock *block = context;
    if (frames > MA5_BRIDGE_FRAMES) return;
    memcpy(block->left, left, frames * sizeof(*left));
    memcpy(block->right, right, frames * sizeof(*right));
    block->frames = frames;
}

int main(void)
{
    Ma5Hw *hw = NULL;
    Ma5Request request;
    PcmBlock pcm = {0};
    uint8_t payload[MA5_BRIDGE_PAYLOAD];
    if (_setmode(_fileno(stdin), _O_BINARY) < 0 || _setmode(_fileno(stdout), _O_BINARY) < 0) return 1;
    while (fread(&request, sizeof(request), 1, stdin) == 1) {
        Ma5Reply reply = {MA5_BRIDGE_VERSION, 1};
        char error[MA5_BRIDGE_ERROR] = {0};
        unsigned arg = request.arg;
        pcm.frames = 0;
        if (request.version != MA5_BRIDGE_VERSION || request.bytes > sizeof(payload) ||
            fread(payload, 1, request.bytes, stdin) != request.bytes) break;
        if (request.op == MA5_OPEN) {
            wchar_t path[1024];
            if (hw || !request.bytes || payload[request.bytes - 1]) break;
            if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)payload, -1, path, 1024))
                strcpy(error, "Cannot convert the Yamaha DLL path to a Wine path");
            else hw = ma5_hw_open_wide(path, NULL, 0, error, sizeof(error));
            if (hw) ma5_hw_set_mixer(hw, capture, &pcm);
            reply.value = hw != NULL;
        } else {
            Ma5HwStats stats;
            if (!hw) break;
            switch (request.op) {
            case MA5_READ: reply.value = ma5_hw_read(hw, arg != 0); break;
            case MA5_WRITE: ma5_hw_write(hw, arg >> 8 != 0, arg & 255); break;
            case MA5_FIFO: ma5_hw_fifo(hw, arg, payload, request.bytes); break;
            case MA5_GENERATE: reply.value = ma5_hw_generate(hw, arg); break;
            case MA5_PUMP: ma5_hw_pump(hw); break;
            case MA5_DETUNE: ma5_hw_set_detune(hw, arg != 0); break;
            case MA5_WATER: ma5_hw_set_water(hw, arg != 0); break;
            case MA5_RESET: ma5_hw_reset(hw); break;
            case MA5_PARAMETER: ma5_hw_parameter(hw, arg >> 8, arg & 255); break;
            case MA5_MEMORY: reply.value = ma5_hw_memory(hw, arg & 0xfffff, arg >> 20 != 0); break;
            case MA5_CURSOR: reply.value = ma5_hw_pcm_cursor(hw, arg); break;
            case MA5_RAM_CHANGED: ma5_hw_ram_changed(hw, arg & 0xffff, arg >> 16); break;
            case MA5_REGISTER: ma5_hw_register(hw, (arg >> 8) & 255, arg & 255, arg >> 16); break;
            case MA5_STRESS: reply.value = (unsigned)ma5_hw_stress(hw); break;
            default: goto done;
            }
            stats = ma5_hw_stats(hw);
            reply.irq = (unsigned)ma5_hw_irq(hw);
            reply.led = ma5_hw_led_token(hw);
            reply.frames = pcm.frames;
            reply.failed = (unsigned)stats.failed;
            reply.callbacks = (uint32_t)stats.callbacks;
            reply.adpcm_refreshes = (uint32_t)stats.adpcm_refreshes;
        }
        if (fwrite(&reply, sizeof(reply), 1, stdout) != 1) break;
        if ((pcm.frames && (fwrite(pcm.left, 2, pcm.frames, stdout) != pcm.frames ||
                            fwrite(pcm.right, 2, pcm.frames, stdout) != pcm.frames)) ||
            (request.op == MA5_OPEN && !reply.value && fwrite(error, sizeof(error), 1, stdout) != 1) ||
            fflush(stdout)) break;
    }
done:
    ma5_hw_close(hw);
    return 0;
}
