/* Linux side of the x86 Yamaha worker. Only the helper loads the DLL;
   phone tones, the final mix and the ALSA device stay in this process. */
#define _GNU_SOURCE
#include "ma5_hw.h"
#include "ma5_bridge.h"
#include "alsa_pcm.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct Ma5Hw {
    int stream, irq, profiling;
    unsigned led;
    pid_t worker;
    HostPcm output;
    FILE *wav;
    Ma5HwStats stats;
    Ma5HwProfile profile;
    Ma5PcmMixer mixer;
    void *mixer_context;
};

static uint64_t now_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static int start_worker(Ma5Hw *h, const char *helper)
{
    posix_spawn_file_actions_t actions;
    char *args[] = {"wine", (char *)helper, NULL};
    int pair[2], error, i;
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair)) return errno;
    for (i = 0; i < 2; ++i) {
        if (pair[i] > STDERR_FILENO) continue;
        /* dup2 must not overwrite the other endpoint if a stdio fd was closed. */
        int fd = fcntl(pair[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        if (fd < 0) { error = errno; goto failed; }
        close(pair[i]);
        pair[i] = fd;
    }
    error = posix_spawn_file_actions_init(&actions);
    if (error) goto failed;
    error = posix_spawn_file_actions_adddup2(&actions, pair[1], STDIN_FILENO);
    if (!error) error = posix_spawn_file_actions_adddup2(&actions, pair[1], STDOUT_FILENO);
    if (!error) error = posix_spawnp(&h->worker, args[0], &actions, NULL, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (!error) {
        close(pair[1]);
        h->stream = pair[0];
        return 0;
    }
failed:
    close(pair[0]);
    close(pair[1]);
    return error;
}

static void stop_worker(Ma5Hw *h)
{
    const struct timespec pause = {0, 10000000};
    unsigned i;
    if (h->stream < 0) return;
    close(h->stream); /* EOF lets the helper terminate the DLL on its worker. */
    for (i = 0; i < 25; ++i) {
        pid_t status = waitpid(h->worker, NULL, WNOHANG);
        if (status > 0 || (status < 0 && errno == ECHILD)) return;
        nanosleep(&pause, NULL);
    }
    kill(h->worker, SIGKILL);
    while (waitpid(h->worker, NULL, 0) < 0 && errno == EINTR) {}
}

static int write_all(int fd, const void *data, size_t bytes)
{
    const uint8_t *p = data;
    while (bytes) {
        ssize_t count = send(fd, p, bytes, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return 0;
        p += count;
        bytes -= (size_t)count;
    }
    return 1;
}

static int read_all(int fd, void *data, size_t bytes, int timeout)
{
    uint8_t *p = data;
    struct pollfd event = {fd, POLLIN, 0};
    while (bytes) {
        ssize_t count;
        int ready = poll(&event, 1, timeout);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return 0;
        count = recv(fd, p, bytes, MSG_DONTWAIT);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return 0;
        p += count;
        bytes -= (size_t)count;
    }
    return 1;
}

static int exchange(Ma5Hw *h, unsigned op, unsigned arg, const void *data, unsigned bytes, Ma5Reply *reply)
{
    const Ma5Request req = {MA5_BRIDGE_VERSION, op, arg, bytes};
    uint64_t start;
    if (!h || h->stats.failed) return 0;
    start = h->profiling ? now_ns() : 0;
    if (bytes > MA5_BRIDGE_PAYLOAD || !write_all(h->stream, &req, sizeof(req)) ||
        !write_all(h->stream, data, bytes) ||
        !read_all(h->stream, reply, sizeof(*reply), op == MA5_OPEN ? 15000 : 1000) ||
        reply->version != MA5_BRIDGE_VERSION || reply->frames > MA5_BRIDGE_FRAMES ||
        (op != MA5_GENERATE && reply->frames)) {
        h->stats.failed = 1;
        return 0;
    }
    if (start) {
        uint64_t elapsed = now_ns() - start;
        ++h->profile.commands;
        if (op == MA5_GENERATE) h->profile.generate_ticks += elapsed;
        else h->profile.other_ticks += elapsed;
    }
    if (op == MA5_RESET) h->irq = 0;
    h->irq |= reply->irq != 0;
    h->led = reply->led;
    h->stats.callbacks = reply->callbacks;
    h->stats.adpcm_refreshes = reply->adpcm_refreshes;
    h->stats.failed = reply->failed != 0;
    return !h->stats.failed;
}

static unsigned command(Ma5Hw *h, unsigned op, unsigned arg)
{
    Ma5Reply reply;
    return exchange(h, op, arg, NULL, 0, &reply) ? reply.value : 0;
}

static void put32(uint8_t *bytes, uint32_t value)
{
    unsigned i;
    for (i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
}

static int wave_header(Ma5Hw *h)
{
    uint8_t header[44] = "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0";
    uint32_t bytes = (uint32_t)(h->stats.frames * 4);
    put32(header + 4, bytes + 36);
    put32(header + 24, 48000);
    put32(header + 28, 192000);
    header[32] = 4;
    header[34] = 16;
    memcpy(header + 36, "data", 4);
    put32(header + 40, bytes);
    return !fseek(h->wav, 0, SEEK_SET) && fwrite(header, 1, sizeof(header), h->wav) == sizeof(header);
}

Ma5Hw *ma5_hw_open(const char *dll, const char *wav, int speakers, char *error, unsigned error_size)
{
    Ma5Hw *h;
    Ma5Reply reply;
    char path[MA5_BRIDGE_PAYLOAD] = "Z:", executable[PATH_MAX], message[MA5_BRIDGE_ERROR];
    const char *helper = getenv("SGHEMU_MA5_HELPER");
    size_t i;
    int result;
    if (!dll || !*dll || !realpath(dll, executable) || strlen(executable) >= sizeof(path) - 2) {
        snprintf(error, error_size, "Cannot read the Yamaha DLL path");
        return NULL;
    }
    for (i = 0; executable[i]; ++i) path[i + 2] = executable[i] == '/' ? '\\' : executable[i];
    if (!helper || !*helper) {
        ssize_t length = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
        if (length >= 0) {
            char *name;
            executable[length] = 0;
            name = strrchr(executable, '/');
            if (name && (size_t)(name - executable) + sizeof("/ma5bridge.exe") <= sizeof(executable)) {
                strcpy(name + 1, "ma5bridge.exe");
                helper = executable;
            }
        }
    }
    if (!helper || access(helper, R_OK)) {
        snprintf(error, error_size, "Yamaha helper missing. Build it with make ma5-helper and install Wine with x86 support.");
        return NULL;
    }
    h = calloc(1, sizeof(*h));
    if (!h) { snprintf(error, error_size, "Cannot allocate Yamaha worker"); return NULL; }
    h->stream = -1;
    result = start_worker(h, helper);
    if (result) {
        snprintf(error, error_size, "Cannot start Wine: %s", strerror(result));
        goto failed;
    }
    if (!exchange(h, MA5_OPEN, 0, path, (unsigned)i + 3, &reply)) {
        snprintf(error, error_size, "Yamaha helper did not respond. Check Wine's x86 support.");
        goto failed;
    }
    if (!reply.value) {
        if (read_all(h->stream, message, sizeof(message), 1000))
            snprintf(error, error_size, "%.*s", (int)sizeof(message), message);
        else snprintf(error, error_size, "Yamaha helper could not load the hardware DLL");
        goto failed;
    }
    if (speakers && !host_pcm_open(&h->output, 2)) {
        snprintf(error, error_size, "Could not open the default ALSA playback device");
        goto failed;
    }
    if (wav) {
        h->wav = fopen(wav, "wb");
        if (!h->wav || !wave_header(h)) {
            snprintf(error, error_size, "Cannot open the Yamaha WAV output");
            goto failed;
        }
    }
    if (error_size) *error = 0;
    return h;
failed:
    ma5_hw_close(h);
    return NULL;
}

int ma5_hw_generate(Ma5Hw *h, unsigned frames)
{
    Ma5Reply reply;
    int16_t left[MA5_BRIDGE_FRAMES], right[MA5_BRIDGE_FRAMES], pcm[MA5_BRIDGE_FRAMES * 2];
    unsigned i, peak;
    if (!exchange(h, MA5_GENERATE, frames, NULL, 0, &reply)) return 0;
    frames = reply.frames;
    if (!reply.value || !frames || !read_all(h->stream, left, frames * sizeof(*left), 1000) ||
        !read_all(h->stream, right, frames * sizeof(*right), 1000)) goto failed;
    if (h->mixer) h->mixer(h->mixer_context, left, right, frames);
    for (i = 0; i < frames; ++i) {
        pcm[2 * i] = left[i];
        pcm[2 * i + 1] = right[i];
        h->stats.nonzero += (unsigned)(left[i] != 0) + (unsigned)(right[i] != 0);
        peak = (unsigned)abs(left[i]); if (peak > h->stats.peak) h->stats.peak = peak;
        peak = (unsigned)abs(right[i]); if (peak > h->stats.peak) h->stats.peak = peak;
    }
    if (h->wav && (h->stats.frames + frames > (UINT32_MAX - 36u) / 4u ||
                   fwrite(pcm, 4, frames, h->wav) != frames)) goto failed;
    h->stats.frames += frames;
    if (!host_pcm_write(&h->output, pcm, frames)) goto failed;
    h->stats.underruns = h->output.underruns;
    return 1;
failed:
    h->stats.failed = 1;
    return 0;
}

void ma5_hw_close(Ma5Hw *h)
{
    if (!h) return;
    stop_worker(h);
    host_pcm_close(&h->output);
    if (h->wav) { wave_header(h); fclose(h->wav); }
    free(h);
}

unsigned ma5_hw_read(Ma5Hw *h, int data) { return command(h, MA5_READ, data != 0); }
void ma5_hw_write(Ma5Hw *h, int data, unsigned value) { command(h, MA5_WRITE, (data ? 256u : 0u) | (value & 255)); }
void ma5_hw_fifo(Ma5Hw *h, unsigned selector, const uint8_t *bytes, unsigned length)
{
    Ma5Reply reply;
    if (bytes && length) exchange(h, MA5_FIFO, selector, bytes, length, &reply);
}
void ma5_hw_pump(Ma5Hw *h) { command(h, MA5_PUMP, 0); }
void ma5_hw_set_detune(Ma5Hw *h, int enabled) { command(h, MA5_DETUNE, enabled != 0); }
void ma5_hw_set_water(Ma5Hw *h, int enabled) { command(h, MA5_WATER, enabled != 0); }
void ma5_hw_pause(Ma5Hw *h, int paused) { if (h) host_pcm_pause(&h->output, paused); }
void ma5_hw_reset(Ma5Hw *h) { command(h, MA5_RESET, 0); }
void ma5_hw_parameter(Ma5Hw *h, unsigned address, unsigned value) { command(h, MA5_PARAMETER, (address << 8) | (value & 255)); }
unsigned ma5_hw_memory(Ma5Hw *h, unsigned address, int full_ram) { return command(h, MA5_MEMORY, address | (full_ram ? 0x100000u : 0u)); }
unsigned ma5_hw_pcm_cursor(Ma5Hw *h, unsigned voice)
{
    Ma5Reply reply;
    return exchange(h, MA5_CURSOR, voice, NULL, 0, &reply) ? reply.value : UINT32_MAX;
}
void ma5_hw_ram_changed(Ma5Hw *h, unsigned address, unsigned length) { command(h, MA5_RAM_CHANGED, (length << 16) | (address & 0xffff)); }
void ma5_hw_register(Ma5Hw *h, unsigned reg, unsigned value, unsigned bank) { command(h, MA5_REGISTER, (bank << 16) | (reg << 8) | (value & 255)); }
unsigned ma5_hw_led_token(Ma5Hw *h) { return h ? h->led : 0; }
int ma5_hw_irq(Ma5Hw *h) { int irq = h ? h->irq : 0; if (h) h->irq = 0; return irq; }
int ma5_hw_stress(Ma5Hw *h) { return (int)command(h, MA5_STRESS, 0); }
Ma5HwStats ma5_hw_stats(Ma5Hw *h) { Ma5HwStats empty = {0}; return h ? h->stats : empty; }
void ma5_hw_set_mixer(Ma5Hw *h, Ma5PcmMixer mixer, void *context) { if (h) { h->mixer = mixer; h->mixer_context = context; } }
void ma5_hw_profile_enable(Ma5Hw *h, int enabled) { if (h) { h->profiling = !!enabled; memset(&h->profile, 0, sizeof(h->profile)); } }
Ma5HwProfile ma5_hw_profile(Ma5Hw *h) { Ma5HwProfile empty = {0}; return h ? h->profile : empty; }
void ma5_hw_diagnostics(Ma5Hw *h)
{
    unsigned reg;
    if (!h) return;
    ma5_hw_write(h, 0, 4);
    ma5_hw_write(h, 1, 0);
    for (reg = 0; reg < 64; ++reg) {
        ma5_hw_write(h, 0, reg);
        printf("HW reg %02x=%02x%s", reg, ma5_hw_read(h, 1), (reg & 7) == 7 ? "\n" : " ");
    }
    ma5_hw_write(h, 0, 0x80);
}
