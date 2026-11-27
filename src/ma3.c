#include "ma3.h"
#include <string.h>

enum { PACKET_DELAY, PACKET_ADDRESS, PACKET_LENGTH, PACKET_RAM, PACKET_PARAMETERS, PACKET_READ };

enum { FRAME_ONLY, IMMEDIATE_WRITE, SEQUENCED_WRITE, READ_REQUEST };

enum { TIMER_A_IRQ = 0x20, FIFO_IRQ = 0x80, FIFO_SERVICE_NS = 2000000 };

#define NO_EVENT UINT64_MAX
#define NS_PER_SECOND UINT64_C(1000000000)

static uint64_t earlier(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

static uint64_t sequence_deadline(const Ma3 *m)
{
    return m->fifo_count && (m->parameter_ram[0x161] & 1u) ? m->sequence_due : NO_EVENT;
}

static uint64_t fifo_deadline(const Ma3 *m)
{
    unsigned threshold = (m->parameter_ram[0x160] & 7u) * 64u;
    if (m->fifo_count > threshold || (m->irq_flags & FIFO_IRQ))
        return NO_EVENT;
    return m->fifo_due < m->now ? m->now : m->fifo_due;
}

static uint64_t device_deadline(const Ma3 *m)
{
    uint64_t due = earlier(m->timers[0].due, m->timers[1].due);
    due = earlier(due, sequence_deadline(m));
    due = earlier(due, fifo_deadline(m));
    if (m->water)
        due = earlier(due, m->water_next);
    return due;
}

static void update_deadline(Ma3 *m)
{
    uint64_t due = device_deadline(m);
    if (m->output && !m->stats.failed)
        due = earlier(due, m->now + FIFO_SERVICE_NS);
    if (due == NO_EVENT || m->ticks_hz == NS_PER_SECOND)
        m->next_event = due;
    else
        m->next_event = (due / NS_PER_SECOND) * m->ticks_hz +
                        ((due % NS_PER_SECOND) * m->ticks_hz + NS_PER_SECOND - 1) / NS_PER_SECOND;
}

static unsigned irq_status(const Ma3 *m)
{
    return m->irq_flags & (m->parameter_ram[0x162] | FIFO_IRQ);
}

static void signal_irq(Ma3 *m)
{
    if ((m->selector & 0x80u) && irq_status(m) && !(m->intc->pending & m->irq_mask)) {
        m->intc->pending |= m->irq_mask;
        ++m->irq_events;
    }
}

static void clear_fifo(Ma3 *m)
{
    m->fifo_head = m->fifo_count = 0;
    memset(&m->fifo_input, 0, sizeof(m->fifo_input));
    memset(&m->delayed, 0, sizeof(m->delayed));
    m->sequence_due = NO_EVENT;
    m->sequence_wait = 0;
}

static void reset_chip(Ma3 *m)
{
    memset(m->parameter_ram, 0, sizeof(m->parameter_ram));
    memset(m->parameter_ram + 0x90, 0x60, 16);
    memset(m->parameter_ram + 0xa0, 0x3c, 16);
    memset(m->parameter_ram + 0x14a, 0x40, 8);
    memset(m->registers, 0, sizeof(m->registers));
    memset(&m->immediate, 0, sizeof(m->immediate));
    m->immediate.phase = PACKET_ADDRESS;
    m->response_ready = m->irq_flags = 0;
    m->timers[0].due = m->timers[1].due = NO_EVENT;
    m->timers[0].period = m->timers[1].period = 0;
    m->sequence_period = m->sequence_quantum_ns;
    m->fifo_due = m->now + FIFO_SERVICE_NS;
    m->water_sample_burst = 0;
    m->led_on = 0;
    m->led_tick = 0;
    clear_fifo(m);
    ma3_synth_reset(&m->synth);
}

static void led_event(Ma3 *m, unsigned value)
{
    static const unsigned colors[] = {2, 4, 1, 3};
    m->led_on = value & 1u;
    m->led_tick = m->bus->ticks;
    ++m->led_events;
    if (m->led_on) {
        unsigned choices[3], count = 0, i;
        if (!m->led_color)
            m->led_color = colors[ma_water_random(&m->led_rng) % 4];
        else {
            for (i = 0; i < 4; ++i)
                if (colors[i] != m->led_color)
                    choices[count++] = colors[i];
            m->led_color = choices[ma_water_random(&m->led_rng) % count];
        }
    }
}

static void configure_timer(Ma3 *m, unsigned index, int restart)
{
    Ma3Timer *timer = &m->timers[index];
    unsigned reload = m->parameter_ram[index ? 0x157 : 0x154];
    unsigned control = m->parameter_ram[index ? 0x158 : 0x155];
    uint64_t tick_ns = UINT64_C(37109) * m->parameter_ram[0x152];
    uint64_t period;
    if (index && (control & 4u))
        tick_ns = m->sequence_period;
    period = (control & 1u) ? tick_ns * reload : 0;
    if (restart || timer->period != period) {
        timer->period = period;
        timer->due = period ? m->now + period : NO_EVENT;
    }
}

static void sequence_set_period(Ma3 *m)
{
    unsigned divider = ((unsigned)m->parameter_ram[0x15e] << 7) | m->parameter_ram[0x15f];
    uint64_t old = m->sequence_period;
    uint64_t period = (uint64_t)(divider ? divider : 1) * m->sequence_quantum_ns;
    uint64_t remaining = m->sequence_wait;
    if (m->sequence_due != NO_EVENT)
        remaining = m->sequence_due > m->now ? m->sequence_due - m->now : 0;
    if (m->water && divider)
        period = (period * m->water_scale + 50) / 100;
    /* Preserve sequence ticks (including a partial tick) on speed changes. */
    remaining = (remaining / old) * period + ((remaining % old) * period + old - 1) / old;
    if (m->sequence_due != NO_EVENT)
        m->sequence_due = m->now + remaining;
    else
        m->sequence_wait = remaining;
    m->sequence_period = period;
    configure_timer(m, 1, 0);
}

static void sequence_reset(Ma3 *m)
{
    /* A delay already taken from the FIFO still belongs to its packet. A
       reset rebases it at the current time. Clearing the FIFO discards it. */
    if (m->delayed.phase == PACKET_ADDRESS && !m->delayed.parts) {
        uint64_t delay = m->delayed.delay * m->sequence_period;
        if (m->parameter_ram[0x161] & 1u)
            m->sequence_due = m->now + delay;
        else
            m->sequence_wait = delay;
    }
}

static void write_parameter(Ma3 *m, unsigned address, unsigned value)
{
    unsigned previous = 0;
    value &= 127u;
    if (address < sizeof(m->parameter_ram)) {
        previous = m->parameter_ram[address];
        m->parameter_ram[address] = (uint8_t)value;
    }
    if (address < MA3_VOICE_END) {
        unsigned offset = address >= 0xe0 ? address - 0xe0 : address;
        /* The fictional water effect changes expression/pitch data, leaving
           addresses, channel selection, key gates and readback intact. */
        if (m->water && (address < 0x90 || address >= 0xe0) && offset % 6 >= 2 && offset % 6 <= 4) {
            uint32_t random = ma_water_random(&m->water_rng);
            if ((random & 7u) == 0) {
                value ^= 1u << ((random >> 8) % 6);
                ++m->water_faults;
            }
        }
        if (address >= 0xb0 && address < 0xe0)
            ++m->unsupported; /* Channel LFO/bend. */
        else
            ma3_synth_write(&m->synth, m->parameter_ram, address, value);
        ++m->commands;
        return;
    }
    switch (address) {
    case 0x146:
        led_event(m, value);
        break;
    case 0x152:
        configure_timer(m, 0, 0);
        configure_timer(m, 1, 0);
        break;
    case 0x154:
    case 0x155:
        configure_timer(m, 0, address == 0x155 && (value & 2u));
        break;
    case 0x157:
    case 0x158:
        configure_timer(m, 1, address == 0x158 && (value & 2u));
        break;
    case 0x15a:
    case 0x15b:
    case 0x15c:
    case 0x15d:
        m->irq_flags |= 1u << (address - 0x15a);
        break;
    case 0x15f: /* The low field latches the divider. */
        sequence_set_period(m);
        break;
    case 0x160:
        if (m->fifo_count > (value & 7u) * 64u)
            m->irq_flags &= ~FIFO_IRQ;
        break;
    case 0x161:
        if ((previous ^ value) & 1u) {
            if (value & 1u) {
                if (m->delayed.phase != PACKET_DELAY)
                    m->sequence_due = m->now + m->sequence_wait;
            } else {
                m->sequence_wait =
                    m->sequence_due != NO_EVENT && m->sequence_due > m->now ? m->sequence_due - m->now : 0;
                m->sequence_due = NO_EVENT;
            }
        }
        if (value & 2u)
            sequence_reset(m);
        break;
    case 0x148:
    case 0x166: /* NOPs issued by the driver. */
    case 0x15e:
    case 0x162:
        break;
    default:
        ++m->unsupported;
        return;
    }
    ++m->commands;
}

static void write_ram(Ma3 *m, unsigned address, unsigned value)
{
    if (m->water && address >= MA3_RAM_BASE && address < MA3_RAM_END) {
        unsigned damaged = ma_water_sample_byte(&m->water_rng, &m->water_sample_burst, value);
        if (damaged != value)
            ++m->water_faults;
        value = damaged;
    }
    ma3_synth_ram(&m->synth, address, (uint8_t)value);
}

static void read_parameter(Ma3 *m, const Ma3Packet *p, unsigned buffer)
{
    unsigned address = p->address;
    unsigned value = 0;
    if (buffer >= 3)
        return;
    if (p->full_ram) {
        if (address >= MA3_RAM_BASE && address < MA3_RAM_END)
            value = m->synth.ram[address - MA3_RAM_BASE];
    } else if (!buffer && address >= 0x14a && address <= 0x151) {
        value = ma3_synth_cursor(&m->synth, address - 0x14a);
    } else if (address < sizeof(m->parameter_ram))
        value = m->parameter_ram[address];
    m->responses[buffer] = (uint8_t)value;
    m->response_ready |= 0x10u << buffer;
    ++m->ram_reads;
}

/* Address, length and delay use least-significant 7-bit fields first,
   terminated by bit 7. A RAM payload's bytes are always literal. */
static int field_byte(Ma3 *m, Ma3Packet *p, uint32_t *field, unsigned limit, unsigned value)
{
    *field |= (value & 127u) << (7 * p->parts++);
    if (value & 128u)
        return 1;
    if (p->parts < limit)
        return 0;
    strcpy(m->error, "Invalid MA-3 packet field");
    return 1;
}

static int packet_byte(Ma3 *m, Ma3Packet *p, unsigned mode, unsigned value)
{
    switch (p->phase) {
    case PACKET_DELAY:
        if (field_byte(m, p, &p->delay, 4, value)) {
            p->parts = 0;
            p->phase = PACKET_ADDRESS;
            if (mode == SEQUENCED_WRITE)
                m->sequence_due = m->now + p->delay * m->sequence_period;
        }
        break;
    case PACKET_ADDRESS:
        if (field_byte(m, p, &p->address, 3, value)) {
            p->full_ram = p->parts == 3;
            p->parts = 0;
            p->phase = mode == READ_REQUEST ? PACKET_READ : p->full_ram ? PACKET_LENGTH : PACKET_PARAMETERS;
        }
        break;
    case PACKET_LENGTH:
        if (field_byte(m, p, &p->length, 2, value)) {
            p->phase = PACKET_RAM;
            return p->length == 0;
        }
        break;
    case PACKET_RAM:
        if (mode != FRAME_ONLY)
            write_ram(m, p->address, value);
        ++p->address;
        return --p->length == 0;
    case PACKET_PARAMETERS:
        if (mode != FRAME_ONLY)
            write_parameter(m, p->address, value);
        ++p->address;
        return (value & 128u) != 0;
    case PACKET_READ:
        read_parameter(m, p, value & 127u);
        return 1;
    }
    return 0;
}

static void run_fifo(Ma3 *m)
{
    while (m->fifo_count && (m->parameter_ram[0x161] & 1u) && !*m->error) {
        unsigned value;
        if (m->sequence_due != NO_EVENT && m->sequence_due > m->now)
            break;
        m->sequence_due = NO_EVENT;
        value = m->fifo[m->fifo_head];
        m->fifo_head = (m->fifo_head + 1) % MA3_FIFO_BYTES;
        --m->fifo_count;
        if (packet_byte(m, &m->delayed, SEQUENCED_WRITE, value))
            memset(&m->delayed, 0, sizeof(m->delayed));
    }
}

static void write_fifo(Ma3 *m, unsigned value)
{
    if (m->fifo_count == MA3_FIFO_BYTES) {
        strcpy(m->error, "MA-3 delayed FIFO overflow");
        return;
    }
    m->fifo[(m->fifo_head + m->fifo_count) % MA3_FIFO_BYTES] = (uint8_t)value;
    ++m->fifo_count;
    if (m->fifo_count > (m->parameter_ram[0x160] & 7u) * 64u)
        m->irq_flags &= ~FIFO_IRQ;
    /* Track incomplete writes for the clear pulse (also while stopped). */
    if (packet_byte(m, &m->fifo_input, FRAME_ONLY, value))
        memset(&m->fifo_input, 0, sizeof(m->fifo_input));
}

static uint64_t frames_at(uint64_t ticks, uint32_t hz)
{
    return (ticks / hz) * AUDIO_RATE + (ticks % hz) * AUDIO_RATE / hz;
}

static void render_to(Ma3 *m, uint64_t ns, uint64_t end_frame)
{
    uint64_t frames;
    m->now = ns;
    if (end_frame <= m->rendered_frames)
        return;
    frames = end_frame - m->rendered_frames;
    m->rendered_frames = end_frame;
    /* A muted frontend still has a running chip. In particular, PCM voice
       cursors and timer/soft IRQs must remain observable from the guest. */
    while (frames) {
        int16_t left[AUDIO_BLOCK], right[AUDIO_BLOCK];
        unsigned count = frames > AUDIO_BLOCK ? AUDIO_BLOCK : (unsigned)frames;
        unsigned i;
        int output = m->output && !m->stats.failed;
        if (!output && !m->synth.active_fm && !m->synth.active_pcm) {
            m->stats.frames += frames;
            break;
        }
        ma3_synth_render(&m->synth, left, right, count);
        frames -= count;
        if (output) {
            int16_t detuned_left[AUDIO_BLOCK + 8], detuned_right[AUDIO_BLOCK + 8];
            int16_t interleaved[(AUDIO_BLOCK + 8) * 2];
            int16_t *l = left, *r = right;
            if (m->detune.enabled) {
                count = ma_detune_pcm(&m->detune, left, right, count, detuned_left, detuned_right,
                                      AUDIO_BLOCK + 8);
                l = detuned_left;
                r = detuned_right;
            }
            ma_water_pcm(&m->water_pcm, l, r, count);
            if (m->mixer)
                m->mixer(m->mixer_context, l, r, count);
            for (i = 0; i < count; ++i) {
                interleaved[i * 2] = l[i];
                interleaved[i * 2 + 1] = r[i];
            }
            if (!audio_output_write(m->output, interleaved, count)) {
                strcpy(m->output_error, "Could not write MA-3 audio output");
                m->stats.failed = 1;
            }
            for (i = 0; i < count; ++i) {
                int a = l[i], b = r[i];
                unsigned peak = (unsigned)(a < 0 ? -a : a), other = (unsigned)(b < 0 ? -b : b);
                if (other > peak)
                    peak = other;
                if (peak > m->stats.peak)
                    m->stats.peak = peak;
                m->stats.nonzero += (unsigned)(a != 0) + (unsigned)(b != 0);
            }
        }
        m->stats.frames += count;
    }
}

void ma3_advance(Ma3 *m, uint64_t ticks)
{
    uint64_t ns, due;
    if (!m->attached || *m->error)
        return;
    ns = ticks;
    if (m->ticks_hz != NS_PER_SECOND)
        ns = (ticks / m->ticks_hz) * NS_PER_SECOND + (ticks % m->ticks_hz) * NS_PER_SECOND / m->ticks_hz;
    if (ns < m->now)
        return;
    while ((due = device_deadline(m)) <= ns && due != NO_EVENT) {
        unsigned i;
        render_to(m, due, frames_at(due, 1000000000));
        for (i = 0; i < 2; ++i) {
            Ma3Timer *timer = &m->timers[i];
            if (timer->due == due) {
                m->irq_flags |= (unsigned)TIMER_A_IRQ << i;
                timer->due += timer->period;
            }
        }
        if (fifo_deadline(m) == due)
            m->irq_flags |= FIFO_IRQ;
        if (m->water && m->water_next == due) {
            uint32_t random = ma_water_random(&m->water_rng);
            m->water_scale = 25 + random % 301;
            sequence_set_period(m);
            ++m->water_faults;
            m->water_next = due + UINT64_C(1000000) * (25 + (random >> 16) % 136);
        }
        run_fifo(m);
        signal_irq(m);
        if (*m->error)
            break;
    }
    /* Convert straight from the guest clock here; rounding to nanoseconds
       first loses a frame at some exact sample boundaries on legacy clocks. */
    render_to(m, ns, frames_at(ticks, m->ticks_hz));
    update_deadline(m);
}

static int response_buffer(unsigned selector)
{
    switch (selector & 0x7fu) {
    case 0x11:
        return 0;
    case 0x22:
        return 1;
    case 0x43:
        return 2;
    default:
        return -1;
    }
}

static void trace_port(Ma3 *m, char direction, unsigned off, unsigned value)
{
    if (m->trace)
        fprintf(m->trace, "%llu %08x %c %u %02x sel=%02x\n", (unsigned long long)m->bus->ticks, m->bus->pc,
                direction, off, value, m->selector);
}

static uint32_t read_ma3(void *context, uint32_t off, unsigned width)
{
    Ma3 *m = context;
    unsigned result, reg = m->selector & 63u;
    int response = response_buffer(m->selector);
    if (width != 1 || (off != 0 && off != 4)) {
        bus_fault_access(m->bus, 0, m->base + off, width, 0, "MA-3 requires byte port access");
        return 0;
    }
    ma3_advance(m, *m->clock);
    ++m->reads;
    if (!off) {
        result = 1u | m->response_ready | (m->control & 0x80u);
        if (m->fifo_count == 0)
            result |= 2u;
        if (m->fifo_count == MA3_FIFO_BYTES)
            result |= 8u;
    } else if (response >= 0) {
        result = m->responses[response];
        m->response_ready &= ~(0x10u << response);
    } else if (reg == 0 && m->bank == 0)
        result = irq_status(m);
    else if (reg == 4)
        result = m->control;
    else
        result = m->registers[m->bank][reg];
    trace_port(m, 'R', off, result);
    return result;
}

static void write_ma3(void *context, uint32_t off, unsigned width, uint32_t value)
{
    Ma3 *m = context;
    unsigned reg = m->selector & 63u;
    if (width != 1 || (off != 0 && off != 4)) {
        bus_fault_access(m->bus, 1, m->base + off, width, value, "MA-3 requires byte port access");
        return;
    }
    ma3_advance(m, *m->clock);
    ++m->writes;
    value &= 255u;
    if (!off) {
        m->selector = value;
        if ((value & 63u) == 2 || (value & 63u) == 3) {
            memset(&m->immediate, 0, sizeof(m->immediate));
            m->immediate.phase = PACKET_ADDRESS;
        }
    } else if (response_buffer(m->selector) < 0) {
        if (reg == 1 && !*m->error) {
            ++m->fifo_bytes;
            write_fifo(m, value);
        } else if ((reg == 2 || reg == 3) && !*m->error) {
            if (reg == 2)
                ++m->fifo_bytes;
            if (packet_byte(m, &m->immediate, reg == 2 ? IMMEDIATE_WRITE : READ_REQUEST, value)) {
                memset(&m->immediate, 0, sizeof(m->immediate));
                m->immediate.phase = PACKET_ADDRESS;
            }
        } else if (reg == 4) {
            unsigned rising = value & ~m->control;
            if (rising & 0x80u)
                reset_chip(m);
            if (rising & 0x20u) {
                ++m->fifo_clears;
                if (m->fifo_input.phase != PACKET_DELAY || m->fifo_input.parts)
                    ++m->partial_drops;
                clear_fifo(m);
            }
            if (rising & 8u)
                sequence_reset(m);
            m->control = value;
            m->bank = value & 3u;
        } else if (reg == 0 && m->bank == 0) {
            m->irq_flags &= ~value;
            if (value & FIFO_IRQ)
                m->fifo_due = m->now + FIFO_SERVICE_NS;
        } else {
            m->registers[m->bank][reg] = (uint8_t)value;
            if (reg == 7 && m->bank == 0)
                m->synth.volume = value & 31u;
        }
    }
    /* Starting, or appending to an already running FIFO, applies due
       commands even if no PCM sample time has elapsed. */
    run_fifo(m);
    signal_irq(m);
    update_deadline(m);
    trace_port(m, 'W', off, value);
}

int ma3_motor_on(const Ma3 *m)
{
    /* XEDK1's direct motor control toggles bit 0 in bank 0 register 0D. */
    return m->attached && (m->registers[0][0x0d] & 1u);
}

int ma3_attach(Ma3 *m, Bus *bus, Intc *intc, uint32_t base, uint32_t irq_mask, uint32_t sequence_quantum_ns)
{
    if (m->attached || !intc->attached || (base & 3u) || !irq_mask || !sequence_quantum_ns ||
        sequence_quantum_ns > 10000)
        return 0;
    memset(m, 0, sizeof(*m));
    m->bus = bus;
    m->intc = intc;
    m->base = base;
    m->irq_mask = irq_mask;
    m->sequence_quantum_ns = sequence_quantum_ns;
    if (!bus_device(bus, base, 8, read_ma3, write_ma3, m))
        return 0;
    m->attached = 1;
    ma3_synth_init(&m->synth);
    reset_chip(m);
    ma3_set_clock(m, &bus->ticks, 1000000000);
    return 1;
}

void ma3_set_clock(Ma3 *m, const uint64_t *ticks, uint32_t ticks_hz)
{
    if (!m->attached || !ticks || ticks_hz < AUDIO_RATE || ticks_hz > NS_PER_SECOND)
        return;
    m->clock = ticks;
    m->ticks_hz = ticks_hz;
    update_deadline(m);
}

void ma3_cold_reset(Ma3 *m)
{
    if (!m->attached)
        return;
    /* Rebase audio at power-on without rendering the time spent powered off. */
    m->now = *m->clock;
    m->rendered_frames = frames_at(*m->clock, m->ticks_hz);
    *m->error = 0;
    reset_chip(m);
    if (m->water)
        m->water_next = m->now + 25000000;
    update_deadline(m);
}

int ma3_open_audio(Ma3 *m, int speakers, const char *wav)
{
    if (!m->attached || m->output) {
        strcpy(m->output_error, m->attached ? "MA-3 audio is already open" : "This model has no MA-3 chip");
        return 0;
    }
    ma3_advance(m, *m->clock);
    m->output = audio_output_open(2, speakers, wav);
    if (!m->output) {
        strcpy(m->output_error, wav ? "Cannot open MA-3 WAV output" : "Cannot open host audio for MA-3");
        return 0;
    }
    memset(&m->stats, 0, sizeof(m->stats));
    *m->output_error = 0;
    update_deadline(m);
    return 1;
}

int ma3_close_audio(Ma3 *m)
{
    int ok;
    if (!m->output)
        return 1;
    ma3_advance(m, *m->clock);
    m->stats.underruns = audio_output_underruns(m->output);
    ok = audio_output_close(m->output);
    m->output = NULL;
    m->stats.failed |= !ok;
    if (!ok && !*m->output_error)
        strcpy(m->output_error, "Could not finish MA-3 audio output");
    update_deadline(m);
    return ok;
}

void ma3_set_mixer(Ma3 *m, Ma3Mixer mixer, void *context)
{
    m->mixer = mixer;
    m->mixer_context = context;
}

void ma3_set_detune(Ma3 *m, int enabled)
{
    if (m->attached)
        ma3_advance(m, *m->clock);
    ma_detune_set(&m->detune, enabled);
}

void ma3_set_water(Ma3 *m, int enabled)
{
    if (m->water == !!enabled)
        return;
    if (m->attached)
        ma3_advance(m, *m->clock);
    m->water = !!enabled;
    m->water_scale = 100;
    m->water_next = m->now;
    m->water_sample_burst = 0;
    ma_water_set(&m->water_pcm, enabled);
    if (m->attached) {
        sequence_set_period(m);
        update_deadline(m);
    }
}

Ma3Stats ma3_stats(const Ma3 *m)
{
    Ma3Stats stats = m->stats;
    if (m->output)
        stats.underruns = audio_output_underruns(m->output);
    return stats;
}

void ma3_destroy(Ma3 *m)
{
    ma3_close_audio(m);
    if (m->trace)
        fclose(m->trace);
    memset(m, 0, sizeof(*m));
}
