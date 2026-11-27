#include "machine.h"
#include "nvram.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

/* Record the guest ports and the corresponding PLL time for waveform comparisons. */
static int run(Machine *m, FILE *timeline, unsigned ms)
{
    uint64_t end = m->clock.nanoseconds + (uint64_t)ms * 1000000u;
    while (m->clock.nanoseconds < end) {
        uint64_t ns = m->clock.nanoseconds;
        uint64_t accesses = m->ma3.reads + m->ma3.writes;
        if (!machine_step(m, m->bringup))
            return 0;
        if (m->ma3.reads + m->ma3.writes != accesses)
            fprintf(timeline, "%llu %llu\n", (unsigned long long)m->bus.ticks, (unsigned long long)ns);
    }
    ma3_advance(&m->ma3, m->clock.nanoseconds);
    return 1;
}

static int number(const char *text, unsigned limit, unsigned *out)
{
    char *end;
    unsigned long value;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (end == text || *end || *text == '-' || errno || value > limit)
        return 0;
    *out = (unsigned)value;
    return 1;
}

int main(int argc, char **argv)
{
    Machine *m;
    FILE *timeline = NULL;
    Ma3Stats stats;
    unsigned boot, tail, key;
    int i, success = 0;
    char path[1024];
    if (argc < 6 || strlen(argv[3]) > sizeof(path) - 32 || !number(argv[4], 3600000, &boot) ||
        !number(argv[5], 3600000, &tail)) {
        fputs("Usage: ringtone_boot_probe MODEL.cfg SNAPSHOT.snv OUTPUT_PREFIX BOOT_MS TAIL_MS [KEY ...]\n",
              stderr);
        return 2;
    }
    for (i = 6; i < argc; ++i)
        if (!number(argv[i], SGH_MAX_KEYS - 1, &key))
            return 2;
    m = calloc(1, sizeof(*m));
    if (!m)
        return 1;
    if (!machine_load(m, argv[1], NULL, stderr) || !nvram_load(m, argv[2]))
        goto done;
    if (!m->clock.attached || !m->lcd.attached) {
        fputs("The capture requires a model with a PLL clock and LCD.\n", stderr);
        goto done;
    }
    snprintf(path, sizeof(path), "%s.wav", argv[3]);
    /* Compare Yamaha output without the separately emulated CPU/DSP beeps. */
    ma3_set_mixer(&m->ma3, NULL, NULL);
    if (!ma3_open_audio(&m->ma3, 0, path))
        goto done;
    snprintf(path, sizeof(path), "%s.log", argv[3]);
    m->ma3.trace = fopen(path, "w");
    snprintf(path, sizeof(path), "%s.time", argv[3]);
    timeline = fopen(path, "w");
    if (!m->ma3.trace || !timeline || !run(m, timeline, boot))
        goto done;
    snprintf(path, sizeof(path), "%s-boot.bmp", argv[3]);
    if (!lcd_save_bmp(&m->lcd, path))
        goto done;

    for (i = 6; i < argc; ++i) {
        key = (unsigned)strtoul(argv[i], NULL, 10);
        printf("Key %u at %.3f seconds, frame %llu\n", key, m->clock.nanoseconds / 1e9,
               (unsigned long long)ma3_stats(&m->ma3).frames);
        machine_key(m, key, 1);
        if (!run(m, timeline, 100))
            goto done;
        machine_key(m, key, 0);
        if (!run(m, timeline, 500))
            goto done;
        snprintf(path, sizeof(path), "%s-key-%d.bmp", argv[3], i - 6);
        if (!lcd_save_bmp(&m->lcd, path))
            goto done;
    }
    if (!run(m, timeline, tail) || !ma3_close_audio(&m->ma3))
        goto done;
    stats = ma3_stats(&m->ma3);
    printf(
        "At end: %.3f seconds, faults %llu; MA-3 reads %llu, IRQs %llu, FIFO bytes %llu, unsupported %llu\n",
        m->clock.nanoseconds / 1e9, (unsigned long long)m->bus.faults, (unsigned long long)m->ma3.reads,
        (unsigned long long)m->ma3.irq_events, (unsigned long long)m->ma3.fifo_bytes,
        (unsigned long long)m->ma3.unsupported);
    printf("PCM frames=%llu nonzero=%llu peak=%u\n", (unsigned long long)stats.frames,
           (unsigned long long)stats.nonzero, stats.peak);
    success = 1;
done:
    if (timeline)
        fclose(timeline);
    if (!success) {
        const char *error = m->cpu.error ? m->cpu.error : *m->error ? m->error : m->ma3.output_error;
        fprintf(stderr, "%s\n", *error ? error : "Ringtone capture failed");
    }
    machine_destroy(m);
    free(m);
    return success ? 0 : 1;
}
