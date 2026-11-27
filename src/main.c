#include "machine.h"
#include "firmware.h"
#include "nvram.h"
#include "userfs.h"
#include "content.h"
#include "host.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int open_audio(Machine *m, int speakers, const char *wav)
{
    if (!ma3_open_audio(&m->ma3, speakers, wav)) {
        fprintf(stderr, "%s\n", m->ma3.output_error);
        return 0;
    }
    m->ma3.trace = fopen("build/ma3-traffic.log", "w");
    if (!m->ma3.trace)
        perror("build/ma3-traffic.log");
    return m->ma3.trace != NULL;
}

int main(int argc, char **argv)
{
    Machine m = {0};
    NvramStore storage = {0};
    char sim_storage[1100] = {0};
    ContentImport content = {0};
    uint64_t budget = 100000, idle_steps = 0, rtc_off_ns = 0, release_power_at = UINT64_MAX;
    C100WakeSource wake_input = C100_WAKE_NONE;
    unsigned i;
    int continuation = 0, result = 0, mmio_trace = 0, until_set = 0, watch_writes_only = 0, budget_set = 0;
    uint32_t until = 0, watch_start = 0, watch_end = 0;
    uint32_t watch_lo[BUS_MAX_WATCH], watch_hi[BUS_MAX_WATCH];
    unsigned watch_count = 0;
    const char *model;
    const char *lcd_output = NULL;
    const char *uart_output = NULL;
    const char *uart_port = NULL;
    SdsUart *uart = NULL;
    const char *import_path = NULL;
    int java_menu = 0, rtc_sync = 0, high_priority = 0, charging = 0, java_extended = 0;
    unsigned battery_percent = 100;
    const char *fs_output = NULL;
    const char *nv_input = NULL, *nv_output = NULL;
    const char *state_directory = NULL; /* Default: model-local, firmware-hashed. */
    const char *audio_wav = NULL;
    int speakers = 0, legacy_audio = 0;
    uint32_t audio_ticks_hz = MA3_DEFAULT_TICKS_HZ;
    int persistent = 1, storage_ready = 0, sim_override = -1;
    FirmwareMatch *matches = NULL;
    unsigned first_argument = 2;
    FILE *trace = NULL, *access_log = NULL, *uart_log = NULL, *log;
    if (argc < 2) {
        puts("sghemu MODEL.cfg | --folder PATH [step-budget] [--lcd-out FILE.bmp]\n"
             "Options: --continue-unmapped --trace --mmio-trace --until ADDRESS --service\n"
             "         --uart-out FILE [--uart-port debug|sds] (default: DEBUG if available)\n"
             "         --rtc-off-ms N (emulated RTC time to spend waiting while powered off)\n"
             "         --wake-with power|charger (one input if still in Standby after the wait)\n"
             "         --nv-load FILE.snv --nv-save FILE.snv (diagnostic import/export)\n"
             "         --state-dir DIRECTORY --no-persist --sim present|none\n"
             "         --audio (MA-3 speakers) --audio-wav FILE.wav (stereo capture)\n"
             "         --audio-ticks-hz N (legacy profiles only, not CPU MHz)\n"
             "         --fs-list FILE.tsv (directory metadata, not file contents)\n"
             "         --import FILE (sound/picture or MIDP 1.0 JAR through the guest)\n"
             "         --java-menu (boot the native installed-MIDlet menu)\n"
             "         --extended-java (optional larger Java working buffers)\n"
             "         --rtc-sync --high-priority --battery 0..100 --charging\n"
             "NVM auto-loads/saves beside the model config in state/. "
             "Run from repo root; see build/unmapped.log.");
        return 2;
    }
    model = argv[1];
    if (!strcmp(model, "--folder")) {
        unsigned count;
        if (argc < 3)
            return 2;
        matches = calloc(FIRMWARE_MAX_MATCHES, sizeof(*matches));
        if (!matches)
            return 1;
        count = firmware_scan(argv[2], "models", matches, FIRMWARE_MAX_MATCHES);
        if (!count) {
            fprintf(stderr, "No complete supported ROM set matches this folder's SHA-256 hashes.\n");
            free(matches);
            return 1;
        }
        for (i = 0; i < count; ++i)
            printf("Detected: %s\n", matches[i].name);
        model = matches[0].model;
        first_argument = 3;
        if (count > 1)
            puts("CLI selects the first match; use the GUI to select another detected version.");
    }
    for (i = first_argument; i < (unsigned)argc; ++i) {
        const char *arg = argv[i];
        int has_value = i + 1 < (unsigned)argc;
        if (!strcmp(arg, "--audio"))
            speakers = 1;
        else if (!strcmp(arg, "--ma5-hw") && has_value) {
            ++i; /* Older scripts supplied a DLL path. Native audio needs no file. */
            legacy_audio = 1;
        } else if (!strcmp(arg, "--audio-wav") && has_value)
            audio_wav = argv[++i];
        else if (!strcmp(arg, "--audio-ticks-hz") && has_value) {
            char *end;
            unsigned long hz = strtoul(argv[++i], &end, 0);
            if (*end || hz < 48000 || hz > 100000000) {
                fprintf(stderr, "Invalid audio instruction-tick rate\n");
                return 2;
            }
            audio_ticks_hz = (uint32_t)hz;
        } else if (!strcmp(arg, "--continue-unmapped"))
            continuation = 1;
        else if (!strcmp(arg, "--no-persist"))
            persistent = 0;
        else if (!strcmp(arg, "--rtc-sync"))
            rtc_sync = 1;
        else if (!strcmp(arg, "--rtc-off-ms") && has_value) {
            char *end;
            const char *ms = argv[++i];
            errno = 0;
            uint64_t wait = strtoull(ms, &end, 10);
            if (ms[0] < '0' || ms[0] > '9' || *end || errno == ERANGE || wait > UINT64_MAX / 1000000u) {
                fprintf(stderr, "Invalid RTC off-time in milliseconds.\n");
                return 2;
            }
            rtc_off_ns = wait * 1000000u;
        } else if (!strcmp(arg, "--wake-with") && has_value) {
            const char *input = argv[++i];
            if (!strcmp(input, "power"))
                wake_input = C100_WAKE_KEY;
            else if (!strcmp(input, "charger"))
                wake_input = C100_WAKE_CHARGER;
            else {
                fprintf(stderr, "--wake-with requires power or charger.\n");
                return 2;
            }
        } else if (!strcmp(arg, "--high-priority"))
            high_priority = 1;
        else if (!strcmp(arg, "--charging"))
            charging = 1;
        else if (!strcmp(arg, "--battery") && has_value) {
            char *end;
            unsigned long n = strtoul(argv[++i], &end, 10);
            if (*end || n > 100) {
                fprintf(stderr, "Battery percentage must be 0..100.\n");
                return 2;
            }
            battery_percent = (unsigned)n;
        } else if (!strcmp(arg, "--java-menu"))
            java_menu = 1;
        else if (!strcmp(arg, "--extended-java"))
            java_extended = 1;
        else if (!strcmp(arg, "--import") && has_value)
            import_path = argv[++i];
        else if (!strcmp(arg, "--service"))
            sgh_service_mode = 1;
        else if (!strcmp(arg, "--sim") && has_value) {
            const char *mode = argv[++i];
            if (!strcmp(mode, "present"))
                sim_override = 1;
            else if (!strcmp(mode, "none"))
                sim_override = 0;
            else {
                fprintf(stderr, "--sim requires present or none\n");
                return 2;
            }
        } else if (!strcmp(arg, "--state-dir") && has_value)
            state_directory = argv[++i];
        else if (!strcmp(arg, "--lcd-out") && has_value)
            lcd_output = argv[++i];
        else if (!strcmp(arg, "--uart-out") && has_value)
            uart_output = argv[++i];
        else if (!strcmp(arg, "--uart-port") && has_value) {
            uart_port = argv[++i];
            if (strcmp(uart_port, "debug") && strcmp(uart_port, "sds")) {
                fprintf(stderr, "--uart-port requires debug or sds.\n");
                return 2;
            }
        } else if (!strcmp(arg, "--fs-list") && has_value)
            fs_output = argv[++i];
        else if (!strcmp(arg, "--nv-load") && has_value)
            nv_input = argv[++i];
        else if (!strcmp(arg, "--nv-save") && has_value)
            nv_output = argv[++i];
        else if (!strcmp(arg, "--mmio-trace"))
            mmio_trace = 1;
        else if (!strcmp(arg, "--until") && has_value) {
            const char *address = argv[++i];
            char *end;
            unsigned long long n = strtoull(address, &end, 0);
            if (*end || end == address || address[0] == '-' || n > UINT32_MAX) {
                if (trace)
                    fclose(trace);
                return 2;
            }
            until = (uint32_t)n;
            until_set = 1;
        } else if (!strcmp(arg, "--trace")) {
            trace = fopen("build/cpu-trace.log", "w");
            if (!trace)
                return 1;
        } else {
            char *end;
            budget = strtoull(arg, &end, 0);
            if (!budget || *end || arg[0] == '-') {
                if (trace)
                    fclose(trace);
                return 2;
            }
            budget_set = 1;
        }
    }
    /* The import trigger sits past storage and UI initialization; give a bare
       --import run enough budget to reach it instead of silently doing nothing. */
    if (import_path && !budget_set)
        budget = 20000000;
    log = fopen("build/unmapped.log", "w");
    if (!log) {
        if (trace)
            fclose(trace);
        return 1;
    }
    if (!machine_load_pair(&m, model, matches ? matches[0].rom[0] : NULL, matches ? matches[0].rom[1] : NULL,
                           log)) {
        fprintf(stderr, "%s\n", m.error);
        result = 1;
        goto done;
    }
    if (high_priority && !host_high_priority(1)) {
        fprintf(stderr, "Could not raise process priority.\n");
        result = 1;
        goto done;
    }
    m.java_extended = java_extended;
    if (java_extended && !m.java_buffer_get) {
        fprintf(stderr, "This model has no extended Java buffer profile.\n");
        result = 1;
        goto done;
    }
    if (rtc_sync && rtc_off_ns) {
        fprintf(stderr, "--rtc-off-ms requires emulated time; omit --rtc-sync.\n");
        result = 2;
        goto done;
    }
    if (rtc_sync && !host_sync_rtc(&m.rtc)) {
        fprintf(stderr, "Host RTC sync unavailable: model has no RTC or date is outside its year range.\n");
        result = 1;
        goto done;
    }
    if (rtc_sync)
        rtc_set_sync(&m.rtc, 1);
    machine_set_battery(&m, battery_percent, charging);
    if (sim_override >= 0) {
        if (!m.sim.attached) {
            fprintf(stderr, "This model has no SIM interface\n");
            result = 1;
            goto done;
        }
        m.sim.present = sim_override;
    }
    if (persistent && !nvram_store_open(&storage, &m, state_directory)) {
        fprintf(stderr, "%s\n", m.error);
        result = 1;
        goto done;
    }
    storage_ready = 1;
    if (storage.active && m.sim.attached) {
        snprintf(sim_storage, sizeof(sim_storage), "%s.sim", storage.path);
        if (!sim_card_load_storage(&m.sim.card, sim_storage)) {
            *sim_storage = 0;
            fprintf(stderr, "%s\n", m.sim.card.error);
            result = 1;
            goto done;
        }
    }
    if (nv_input && !(persistent ? nvram_store_import(&storage, &m, nv_input) : nvram_load(&m, nv_input))) {
        fprintf(stderr, "%s\n", m.error);
        result = 1;
        goto done;
    }
    if (import_path && !storage.active) {
        fprintf(
            stderr,
            "Content import needs the persistent phone directory; boot once so the guest initializes it, then import.\n");
        result = 1;
        goto done;
    }
    if (import_path && !content_import_open(&content, &m, import_path)) {
        fprintf(stderr, "%s\n", content.error);
        result = 1;
        goto done;
    }
    if (import_path && !budget_set && content.type == 10)
        budget = 1800000000;
    if (java_menu && import_path) {
        fprintf(stderr, "Use --java-menu or --import, not both.\n");
        result = 1;
        goto done;
    }
    if (java_menu && !content_java_menu(&content, &m)) {
        fprintf(stderr, "%s\n", content.error);
        result = 1;
        goto done;
    }
    if (java_menu && !budget_set)
        budget = 600000000;
    if (m.ma3.attached && !m.clock.attached)
        ma3_set_clock(&m.ma3, &m.cpu.cycles, audio_ticks_hz);
    if (legacy_audio) {
        fputs("--ma5-hw is deprecated; using native MA-3 audio.\n", stderr);
        if (!audio_wav)
            speakers = 1;
    }
    if (speakers || audio_wav) {
        if (!open_audio(&m, speakers, audio_wav)) {
            result = 1;
            goto done;
        }
        if (m.clock.attached)
            puts("Native MA-3: PLL/core-cycle emulated time, stereo PCM 48000 Hz");
        else
            printf("Native MA-3: %u instruction ticks/sec, stereo PCM 48000 Hz\n", audio_ticks_hz);
    }
    if (storage.active)
        printf("Persistent NVM: %s%s\n", storage.path,
               storage.provision ? " (first-boot guest provisioning)" : "");
    if (m.bringup) {
        continuation = 1;
        puts(
            m.periodic_time
                ? "Experimental bring-up: inferred handoff state and emulated-time frame IRQ source; unknown MMIO continues."
                : "Experimental bring-up: inferred handoff state and instruction-tick frame IRQ source; unknown MMIO continues.");
    }
    printf("Model: %s\n", m.name);
    if (m.unplaced_rom)
        printf("Second image: %s (%zu bytes, guest address unresolved)\n", m.unplaced_path, m.unplaced_size);
    if (mmio_trace) {
        access_log = fopen("build/mmio-trace.log", "w");
        if (!access_log) {
            result = 1;
            goto done;
        }
        m.bus.access_log = access_log;
        m.bus.watch_start = 0x00040000;
        m.bus.watch_end = 0x00042000;
        watch_start = 0x00040000;
        watch_end = 0x00042000;
    } else {
        /* MA3_ACCESS_WATCH=start,end[;start,end...] arms the same watch over
           arbitrary guest RAM, writes only, so a RAM field can be attributed to
           the guest PC that wrote it. Same instrument src/gui.c exposes to the
           frontend. */
        const char *range = getenv("MA3_ACCESS_WATCH");
        if (range && *range) {
            access_log = fopen("build/ram-watch.log", "w");
            if (!access_log) {
                result = 1;
                goto done;
            }
            if (!bus_watch_parse(&m.bus, range)) {
                fclose(access_log);
                access_log = NULL;
            } else {
                unsigned r;
                watch_start = m.bus.watch_start;
                watch_end = m.bus.watch_end;
                watch_writes_only = 1;
                watch_count = m.bus.watch_count;
                for (r = 0; r < watch_count; ++r) {
                    watch_lo[r] = m.bus.watch_lo[r];
                    watch_hi[r] = m.bus.watch_hi[r];
                }
                m.bus.access_log = access_log;
                m.bus.watch_writes_only = 1;
            }
        }
    }
    if (uart_output) {
        int debug_port = uart_port ? !strcmp(uart_port, "debug") : m.debug_uart.attached;
        uart = debug_port ? &m.debug_uart : &m.sds_uart;
        if (!uart->attached) {
            fprintf(stderr, "The selected serial port is not mapped.\n");
            result = 1;
            goto done;
        }
        uart_log = fopen(uart_output, "wb");
        if (!uart_log) {
            fprintf(stderr, "Cannot open UART output: %s\n", uart_output);
            result = 1;
            goto done;
        }
        uart->tx_log = uart_log;
    }
    while (m.cpu.instructions < budget - idle_steps) {
        if (machine_powered_off(&m)) {
            if (import_path || (java_menu && !content.done))
                break;
            uint64_t off_start = m.c100.off_ns;
            int woke = machine_advance_off(&m, rtc_off_ns);
            rtc_off_ns -= m.c100.off_ns - off_start;
            if (!woke && wake_input != C100_WAKE_NONE) {
                if (wake_input == C100_WAKE_KEY) {
                    machine_key(&m, SGH_KEY_POWER, 1);
                    woke = !machine_powered_off(&m);
                    if (woke)
                        release_power_at = m.clock.nanoseconds + m.c100.key_hold_ns;
                } else {
                    machine_set_battery(&m, battery_percent, 1);
                    woke = machine_advance_off(&m, 0);
                }
                wake_input = C100_WAKE_NONE;
            }
            if (!woke)
                break;
            if (m.c100.wake_source == C100_WAKE_CHARGER)
                charging = 1;
            printf("%s powered on the phone; cold reset.\n",
                   m.c100.wake_source == C100_WAKE_KEY ? "Power key"
                   : m.c100.wake_source == C100_WAKE_CHARGER ? "Charger insertion"
                                                            : "RTC alarm");
            if (m.cpu.halted) {
                result = 1;
                break;
            }
        }
        if (release_power_at != UINT64_MAX && m.clock.nanoseconds >= release_power_at) {
            machine_key(&m, SGH_KEY_POWER, 0);
            release_power_at = UINT64_MAX;
            if (machine_powered_off(&m))
                continue;
        }
        if (rtc_sync && !((m.cpu.instructions + idle_steps) & 65535u)) {
            rtc_set_sync(&m.rtc, 1);
            (void)host_sync_rtc(&m.rtc);
        }
        int provision = nvram_store_step(&storage, &m);
        if (!provision) {
            fprintf(stderr, "%s\n", m.error);
            result = 1;
            break;
        }
        if (provision == 2) {
            int initialize_defaults = storage.provision && storage.stage == 3;
            puts(initialize_defaults
                     ? "Guest format and initialization marker saved; cold-resetting to create defaults."
                     : "Guest UI defaults complete and saved; cold-resetting for normal startup.");
            if (*sim_storage && !sim_card_save_storage(&m.sim.card, sim_storage)) {
                fprintf(stderr, "%s\n", m.sim.card.error);
                result = 1;
                break;
            }
            nvram_store_release(&storage);
            if (!machine_load_pair(&m, model, matches ? matches[0].rom[0] : NULL,
                                   matches ? matches[0].rom[1] : NULL, log) ||
                !nvram_store_open(&storage, &m, state_directory)) {
                fprintf(stderr, "%s\n", m.error);
                storage_ready = 0;
                result = 1;
                break;
            }
            if (*sim_storage && !sim_card_load_storage(&m.sim.card, sim_storage)) {
                *sim_storage = 0;
                fprintf(stderr, "%s\n", m.sim.card.error);
                result = 1;
                break;
            }
            storage.defaults_boot = initialize_defaults;
            idle_steps = 0;
            release_power_at = UINT64_MAX;
            if (uart)
                uart->tx_log = uart_log;
            if (sim_override >= 0)
                m.sim.present = sim_override;
            m.java_extended = java_extended;
            machine_set_battery(&m, battery_percent, charging);
            if (m.ma3.attached && !m.clock.attached)
                ma3_set_clock(&m.ma3, &m.cpu.cycles, audio_ticks_hz);
            if ((speakers || audio_wav) && !open_audio(&m, speakers, audio_wav)) {
                result = 1;
                break;
            }
            if (access_log) {
                unsigned r;
                m.bus.access_log = access_log;
                for (r = 0; r < watch_count; ++r) {
                    m.bus.watch_lo[r] = watch_lo[r];
                    m.bus.watch_hi[r] = watch_hi[r];
                }
                m.bus.watch_count = watch_count;
                m.bus.watch_start = watch_start;
                m.bus.watch_end = watch_end;
                m.bus.watch_writes_only = watch_writes_only;
            }
            continue;
        }
        if (until_set && m.cpu.r[15] == until) {
            printf("Address breakpoint reached: %08x\n", until);
            break;
        }
        if (!m.cellular.stage && (m.battery.stage || !(content.active && !content.done && !content.failed)) &&
            !battery_step(&m.battery, &m.cpu)) {
            fprintf(stderr, "Guest battery event failed.\n");
            result = 1;
            break;
        }
        if (!m.cellular.stage && !m.battery.stage && content.active && !content.done && !content.failed) {
            int imported = content_import_step(&content, &m);
            if (!imported) {
                fprintf(stderr, "%s\n", content.error);
                result = 1;
                break;
            }
            if (imported == 2 && content.type == 10)
                puts(content.size ? "Java installer started; wait for the handset installation result."
                                  : "Native Java menu started.");
            if (imported == 2 && content.type != 10)
                printf(
                    "Content import: guest accepted \"%s\" (%u bytes); the flash task writes it as the boot continues.\n",
                    content.name, content.size);
        }
        if (trace)
            fprintf(trace, "%llu %08x CPSR=%08x R0=%08x R1=%08x SP=%08x\n",
                    (unsigned long long)m.cpu.instructions, m.cpu.r[15], m.cpu.cpsr, m.cpu.r[0], m.cpu.r[1],
                    m.cpu.r[13]);
        if (machine_sleeping(&m))
            ++idle_steps;
        const uint64_t reset_count = m.c100.reset_count;
        if (!machine_step(&m, continuation)) {
            if (machine_powered_off(&m))
                continue;
            result = 1;
            break;
        }
        if (m.c100.reset_count != reset_count && content.active && (import_path || !content.done)) {
            fputs("Content import: phone reset before the import finished.\n", stderr);
            storage_ready = 0;
            nv_output = NULL;
            result = 1;
            break;
        }
    }
    if (import_path && machine_powered_off(&m)) {
        fputs("Content import: phone entered Standby before the import finished.\n", stderr);
        storage_ready = 0;
        nv_output = NULL;
        result = 1;
    } else if (content.active && !content.done && !content.failed) {
        fprintf(
            stderr,
            "Content import: the guest import trigger was not reached; give the boot enough instructions to pass storage initialization.\n");
        result = 1;
    }
    if (uart && uart->nanoseconds)
        sds_uart_advance(uart);
    printf("instructions=%llu scheduler_ticks=%llu PC=%08x CPSR=%08x unmapped/ROM-faults=%llu\n",
           (unsigned long long)m.cpu.instructions, (unsigned long long)m.cpu.cycles, m.cpu.r[15], m.cpu.cpsr,
           (unsigned long long)m.bus.faults);
    if (idle_steps)
        printf("CPU sleep: idle-steps=%llu, %s\n", (unsigned long long)idle_steps,
               machine_sleeping(&m) ? "sleeping" : "awake");
    if (m.clock.attached)
        printf("PLL: ARM=%u Hz DSP=%u Hz core-cycles=%llu emulated-time=%.6f seconds%s\n", m.clock.arm_hz,
               m.clock.dsp_hz, (unsigned long long)m.cpu.clock_cycles,
               (double)m.clock.nanoseconds / 1000000000.0, m.clock.unknown ? " (unrecognized divider)" : "");
    {
        char status[256];
        if (arm7_fault_status(&m.cpu, status, sizeof(status)) > 0)
            puts(status);
        if (sds_uart_tx_status(&m.debug_uart, "DEBUG", status, sizeof(status)) > 0)
            puts(status);
        if (sds_uart_tx_status(&m.sds_uart, "SDS", status, sizeof(status)) > 0)
            puts(status);
    }
    if (m.lcd.attached)
        printf("LCD display=%s commands=%llu pixel_writes=%llu; IRQ events=%llu\n",
               m.lcd.enabled ? "on" : "off", (unsigned long long)m.lcd.commands,
               (unsigned long long)m.lcd.writes, (unsigned long long)m.intc.events);
    {
        char status[224];
        if (machine_cx805_status(&m, status, sizeof(status)) > 0)
            puts(status);
    }
    for (i = 0; i < 2; ++i)
        if (m.timers[i].nanoseconds)
            printf("Timer %u: %llu IRQ expirations\n", i + 1, (unsigned long long)m.timers[i].expirations);
    if (m.rtc.alarm_events || m.lp_timer.wake_events)
        printf("Wake sources: RTC alarms=%llu, sleep timer=%llu\n", (unsigned long long)m.rtc.alarm_events,
               (unsigned long long)m.lp_timer.wake_events);
    if (m.cpu.error)
        fprintf(stderr, "STOP PC=%08x opcode=%08x: %s\n", m.cpu.last_pc, m.cpu.last_opcode, m.cpu.error);
    if (m.sim.attached)
        printf("SIM: %s, resets=%llu APDUs=%llu RX=%llu TX=%llu DMA=%llu\n",
               m.sim.present ? "synthetic card inserted" : "removed", (unsigned long long)m.sim.resets,
               (unsigned long long)m.sim.card.commands, (unsigned long long)m.sim.received,
               (unsigned long long)m.sim.transmitted, (unsigned long long)m.sim.dma_bytes);
    if (m.ma3.output) {
        Ma3Stats stats;
        ma3_close_audio(&m.ma3);
        stats = ma3_stats(&m.ma3);
        printf(
            "MA-3: reads=%llu writes=%llu FIFO=%llu IRQ=%llu; PCM frames=%llu nonzero=%llu peak=%u host-underruns=%llu%s\n",
            (unsigned long long)m.ma3.reads, (unsigned long long)m.ma3.writes,
            (unsigned long long)m.ma3.fifo_bytes, (unsigned long long)m.ma3.irq_events,
            (unsigned long long)stats.frames, (unsigned long long)stats.nonzero, stats.peak,
            (unsigned long long)stats.underruns, stats.failed ? " FAILED" : "");
        printf("MA-3: RAM reads=%llu commands=%llu unsupported=%llu FIFO queued=%u\n",
               (unsigned long long)m.ma3.ram_reads, (unsigned long long)m.ma3.commands,
               (unsigned long long)m.ma3.unsupported, m.ma3.fifo_count);
        if (stats.failed) {
            fprintf(stderr, "%s\n", m.ma3.output_error);
            result = 1;
        }
    }
    for (i = 0; i < m.flash_count; ++i)
        printf("Flash %08x: programs=%llu erases=%llu last_program=%08x last_erase=%08x\n", m.flashes[i].base,
               (unsigned long long)m.flashes[i].programs, (unsigned long long)m.flashes[i].erases,
               m.flashes[i].last_program, m.flashes[i].last_erase);
    for (i = 0; i < m.irom.count; ++i)
        printf("Internal-ROM HLE %08x: %llu calls%s\n", m.irom.services[i].address,
               (unsigned long long)m.irom.services[i].calls,
               m.irom.services[i].kind == IROM_VERSION ? " (caller-specific version query; no BIST results)"
                                                       : " (memcpy)");
    if (lcd_output && (!m.lcd.attached || !lcd_save_bmp(&m.lcd, lcd_output))) {
        fprintf(stderr, "Cannot save LCD framebuffer.\n");
        result = 1;
    }
    if (nv_output && !nvram_save(&m, nv_output)) {
        fprintf(stderr, "%s\n", m.error);
        result = 1;
    }
    if (fs_output) {
        UserDirectory directory;
        if (!userfs_directory(&m, &directory)) {
            fprintf(stderr, "Phone filesystem: %s\n", directory.error);
            result = 1;
        } else if (!userfs_export_directory(&directory, fs_output)) {
            fprintf(stderr, "Cannot export phone file directory.\n");
            result = 1;
        } else
            printf("Phone filesystem: %u files, %llu listed bytes; directory exported to %s\n",
                   directory.count, (unsigned long long)directory.listed_bytes, fs_output);
    }
    if (m.bus.fault)
        printf("Stopped on unknown hardware at PC=%08x (%c%u at %08x); see build/unmapped.log.\n",
               m.bus.last_pc, m.bus.last_write ? 'W' : 'R', m.bus.last_width * 8, m.bus.last_address);
done:
    if (storage_ready && !nvram_store_flush(&storage, &m)) {
        fprintf(stderr, "%s\n", m.error);
        result = 1;
    }
    if (*sim_storage && !sim_card_save_storage(&m.sim.card, sim_storage)) {
        fprintf(stderr, "%s\n", m.sim.card.error);
        result = 1;
    }
    if (uart_log) {
        int failed = ferror(uart_log);
        if (fclose(uart_log) || failed) {
            fprintf(stderr, "Cannot write UART output: %s\n", uart_output);
            result = 1;
        }
    }
    nvram_store_release(&storage);
    content_import_release(&content);
    if (trace)
        fclose(trace);
    machine_destroy(&m);
    if (access_log)
        fclose(access_log);
    fclose(log);
    free(matches);
    return result;
}
