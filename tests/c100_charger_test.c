/* Optional real-ROM test. Reads a completed snapshot; never saves NVM/SIM. */
#include "machine.h"
#include "nvram.h"
#include <stdlib.h>
#include <string.h>

static int run_until(Machine *m, uint64_t deadline)
{
    while (m->clock.nanoseconds < deadline)
        if (machine_run(m, 256, 1) != 256)
            return 0;
    return 1;
}

static int shutdown_on_charge(Machine *m, unsigned percent)
{
    if (!machine_start_standby(m))
        return 0;
    machine_set_battery(m, percent, 0);
    machine_key(m, SGH_KEY_POWER, 1);
    if (!run_until(m, m->c100.key_hold_ns) || !(m->pmic.regs[PMIC_POWER_CONTROL] & PMIC_POWER_HOLD))
        return 0;
    machine_key(m, SGH_KEY_POWER, 0);
    if (!run_until(m, UINT64_C(12000000000)) || !m->cellular.boot_ready)
        return 0;

    machine_set_battery(m, percent, 1);
    if (!run_until(m, UINT64_C(18000000000)))
        return 0;
    uint64_t reset_count = m->c100.reset_count;
    machine_key(m, SGH_KEY_POWER, 1);
    if (!run_until(m, UINT64_C(20600000000)))
        return 0;
    machine_key(m, SGH_KEY_POWER, 0);
    while (m->c100.reset_count == reset_count && m->clock.nanoseconds < UINT64_C(35000000000))
        if (machine_run(m, 256, 1) != 256)
            return 0;
    if (m->c100.reset_count != reset_count + 1 || !m->battery.charging)
        return 0;
    printf("Charger-connected shutdown reset at %.3f s\n", m->clock.nanoseconds / 1e9);
    return 1;
}

static int charging_frame_advances(Machine *m)
{
    if (!m->lcd.enabled)
        return 0;
    size_t bytes = (size_t)m->width * m->height * sizeof(*m->pixels);
    uint32_t *frame = malloc(bytes);
    int changed = 0;
    if (!frame)
        return 0;
    memcpy(frame, m->pixels, bytes);
    for (unsigned i = 0; i < 8; ++i) {
        if (!run_until(m, m->clock.nanoseconds + 250000000))
            break;
        if (m->lcd.enabled && memcmp(frame, m->pixels, bytes)) {
            changed = 1;
            break;
        }
    }
    free(frame);
    return changed;
}

int main(int argc, char **argv)
{
    Machine *m;
    unsigned i, percent;
    int result = 1;
    char path[1024];
    int shutdown = argc == 6 && !strcmp(argv[5], "--shutdown");
    if (argc != 5 && !shutdown) {
        fprintf(stderr, "Usage: c100_charger_test MODEL.cfg READ_ONLY.snv PERCENT OUTPUT_PREFIX [--shutdown]\n");
        return 2;
    }
    percent = (unsigned)strtoul(argv[3], NULL, 10);
    if (percent > 100)
        return 2;
    m = calloc(1, sizeof(*m));
    if (!m)
        return 1;
    if (!machine_load(m, argv[1], NULL, NULL) || !nvram_load(m, argv[2]) || !m->battery.complete_port)
        goto done;
    if (shutdown) {
        if (!shutdown_on_charge(m, percent))
            goto done;
    } else
        machine_set_battery(m, percent, 1);
    for (i = 0; i < 400000000;) {
        unsigned boundary = i < 150000000 ? 150000000 : 400000000;
        unsigned n = boundary - i;
        if (n > 256)
            n = 256;
        if (machine_run(m, n, 1) != n)
            goto done;
        i += n;
        if (i == 150000000) {
            snprintf(path, sizeof(path), "%s-before.bmp", argv[4]);
            if (!lcd_save_bmp(&m->lcd, path) || (shutdown && !charging_frame_advances(m)))
                goto done;
        }
    }
    snprintf(path, sizeof(path), "%s-after.bmp", argv[4]);
    if (!lcd_save_bmp(&m->lcd, path))
        goto done;
    /* Firmware stopping the active-low charger output acknowledges completion.
       No PC, native mode variable, animation resource or pixels are patched. */
    if (m->cpu.halted || !m->intc.events || !m->dsp_mailbox.conversions || m->lcd.writes < 128 * 128)
        goto done;
    if (percent == 100) {
        if (!m->battery.complete || !(m->battery.complete_port->output & (1u << m->battery.enable_bit)))
            goto done;
    } else if (m->battery.complete || (m->battery.complete_port->output & (1u << m->battery.enable_bit)))
        goto done;
    printf("PASS %s battery=%u done=%u charger_output=%02x wakeups=%llu LCD=%llu\n", m->name, percent,
           m->battery.complete, m->battery.complete_port->output, (unsigned long long)m->lp_timer.wake_events,
           (unsigned long long)m->lcd.writes);
    result = 0;
done:
    if (result)
        fprintf(stderr, "FAIL PC=%08x done=%u output=%02x IRQ=%llu ADC=%llu LCD=%llu machine=%s CPU=%s\n",
                m->cpu.r[15], m->battery.complete,
                m->battery.complete_port ? m->battery.complete_port->output : 0,
                (unsigned long long)m->intc.events, (unsigned long long)m->dsp_mailbox.conversions,
                (unsigned long long)m->lcd.writes, m->error, m->cpu.error ? m->cpu.error : "");
    machine_destroy(m);
    free(m);
    return result;
}
