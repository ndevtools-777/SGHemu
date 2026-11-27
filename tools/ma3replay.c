/* Replay a guest MA-3 port trace without loading firmware. The trace's time
   column is in the specified ticks/sec (1000000000 for a nanosecond trace). */
#include "ma3.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

static Bus bus;

int main(int argc, char **argv)
{
    Ma3 m = {0};
    Intc intc = {0};
    FILE *input = NULL;
    Ma3Stats stats;
    char line[512], direction;
    unsigned long long cycle, first = 0, last = 0;
    uint64_t elapsed = 0;
    unsigned rate = MA3_DEFAULT_TICKS_HZ, off, value, pc;
    int success = 0, have_event = 0;
    if (argc < 3 || argc > 5) {
        fputs("Usage: ma3replay TRACE.log OUTPUT.wav [TICKS_HZ] [OUTPUT_TRACE.log]\n", stderr);
        return 2;
    }
    if (argc > 3) {
        char *end;
        unsigned long n;
        errno = 0;
        n = strtoul(argv[3], &end, 0);
        if (end == argv[3] || *end || errno || n < AUDIO_RATE || n > 1000000000)
            return 2;
        rate = (unsigned)n;
    }
    input = fopen(argv[1], "r");
    if (!input) {
        perror(argv[1]);
        return 1;
    }
    intc.attached = 1;
    if (!ma3_attach(&m, &bus, &intc, 0x200000, 0x800000, 1157))
        goto done;
    ma3_set_clock(&m, &elapsed, rate);
    if (!ma3_open_audio(&m, 0, argv[2]))
        goto done;
    if (argc > 4) {
        m.trace = fopen(argv[4], "w");
        if (!m.trace) {
            perror(argv[4]);
            goto done;
        }
    }
    while (fgets(line, sizeof(line), input)) {
        if (sscanf(line, "%llu %x %c %u %x", &cycle, &pc, &direction, &off, &value) != 5)
            continue;
        if ((direction != 'R' && direction != 'W') || (off != 0 && off != 4) || cycle < last)
            goto done;
        if (!have_event) {
            first = cycle;
            have_event = 1;
        }
        elapsed = cycle - first;
        bus.ticks = cycle;
        bus.pc = pc;
        if (direction == 'W')
            bus_write(&bus, 0x200000 + off, 1, value);
        else
            bus_read(&bus, 0x200000 + off, 1);
        if (*m.error)
            goto done;
        last = cycle;
    }
    if (ferror(input) || !have_event)
        goto done;
    elapsed += (uint64_t)rate * 2; /* Render the release and any remaining delayed commands. */
    if (!ma3_close_audio(&m) || *m.error)
        goto done;
    stats = ma3_stats(&m);
    printf("PCM frames=%llu nonzero=%llu peak=%u; RAM reads=%llu commands=%llu unsupported=%llu\n",
           (unsigned long long)stats.frames, (unsigned long long)stats.nonzero, stats.peak,
           (unsigned long long)m.ram_reads, (unsigned long long)m.commands,
           (unsigned long long)m.unsupported);
    success = 1;
done:
    fclose(input);
    if (!success) {
        const char *error = *m.error ? m.error : m.output_error;
        fprintf(stderr, "%s\n", *error ? error : "MA-3 trace replay failed");
    }
    ma3_destroy(&m);
    bus_destroy(&bus);
    return success ? 0 : 1;
}
