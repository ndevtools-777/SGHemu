#include "sds_uart.h"
#include <string.h>
/* The original compatibility port is modelled from the guest's USART driver
   (`USARTOpen` 0115BFB8): `+0` control (bit0 enable, bit1 arm; zeroed while
   disabled then enabled), `+2` format, `+4` TX data, `+6` mode/status, `+8`
   control B, `+0xA` format B, `+0xC` RX data, divisor table at `+0x20..`. The
   factory probe opens it at `0x40200`; `USARTRead`/`USARTWrite` are software
   ring copies, so the bytes are moved by the general DMA controller
   (`sds_dma.c`), not by this block. The INTC bits below belong to the SDS port
   instance (`0x41200`, opened by the RF test); the factory boot never arms them.
   The cable echo is gated on the control enable bit so `USARTOpen`'s reset
   (which zeroes `+4` while disabled) does not inject a spurious byte. */
#define SDS_IRQ_TX (1u << 14) /* SDSServiceSerialTxBuffer: load next TX byte */
#define SDS_IRQ_RX (1u << 16) /* SDSServiceSerialRxBuffer: store received byte */
#define DEBUG_IRQ_TX (1u << 18)
#define UART_TX_CONTROL 0x00u
#define UART_TX_DIVISOR 0x02u
#define SDS_TX_DATA 0x04u
#define UART_TX_STATUS 0x06u
#define SDS_RX_DATA 0x0cu
#define SDS_WINDOW 0x30u
#define UART_WINDOW 0x14u

enum {
    UART_TX_ENABLE = 1u << 0,
    UART_TX_PIO = 1u << 1,
    UART_TX_SEVEN_BITS = 1u << 2,
    UART_TX_PARITY = 1u << 3,
    UART_TX_TWO_STOP_BITS = 1u << 6,
    UART_TX_IRQ_ON_IDLE = 1u << 9,
    UART_TX_FORMAT_MASK = 3u << 10,
    UART_TX_HOLDING_BUSY = 1u << 0
};

static void update_tx_request(SdsUart *d)
{
    uint16_t control = d->regs[UART_TX_CONTROL / 2];
    int request = d->clock_on && (control & UART_TX_ENABLE) && (control & UART_TX_PIO) && !d->holding_full &&
                  (!(control & UART_TX_IRQ_ON_IDLE) || d->tx_due == UINT64_MAX);
    if (request && !d->tx_request)
        d->intc->pending |= d->tx_irq;
    d->tx_request = request;
}

static void start_frame(SdsUart *d, uint64_t now)
{
    uint16_t control = d->regs[UART_TX_CONTROL / 2];
    unsigned divisor = d->regs[UART_TX_DIVISOR / 2];
    unsigned data_bits = (control & UART_TX_SEVEN_BITS) ? 7 : 8;
    unsigned frame_bits = 1 + data_bits + 1; /* Start bit, data, stop bit. */
    uint64_t scaled;

    if (!d->clock_on || !d->holding_full || d->tx_due != UINT64_MAX || !divisor ||
        (control & UART_TX_FORMAT_MASK))
        return;
    if (control & UART_TX_PARITY)
        ++frame_bits;
    if (control & UART_TX_TWO_STOP_BITS)
        ++frame_bits;
    scaled = (uint64_t)frame_bits * divisor * 1000000000u;
    d->tx_due = now + (scaled + d->reference_hz - 1) / d->reference_hz;
    d->shift_byte = (uint8_t)(d->regs[SDS_TX_DATA / 2] & ((1u << data_bits) - 1));
    d->frame_visible = d->tx_pin;
    d->holding_full = 0;
}

void sds_uart_advance(SdsUart *d)
{
    uint64_t now = *d->nanoseconds;
    while (d->tx_due != UINT64_MAX && d->tx_due <= now) {
        uint64_t completed = d->tx_due;
        d->tx_due = UINT64_MAX;
        ++d->transmits;
        if (d->frame_visible) {
            ++d->pin_transmits;
            if (d->tx_log)
                fputc(d->shift_byte, d->tx_log);
        }
        start_frame(d, completed);
        update_tx_request(d);
    }
}

int sds_uart_tx_status(const SdsUart *d, const char *name, char *text, size_t size)
{
    if (!d->attached)
        return 0;
    if (!d->nanoseconds)
        return snprintf(text, size, "%s UART @%08x: TX bytes=%llu (service mode)", name, d->base,
                        (unsigned long long)d->transmits);
    unsigned control = d->regs[UART_TX_CONTROL / 2];
    unsigned divisor = d->regs[UART_TX_DIVISOR / 2];
    return snprintf(text, size,
                    "%s UART @%08x: clock=%s, TX pin=%s, TX=%s (control=%04x), divisor=%u, baud=%u%s, frames=%llu, pin bytes=%llu",
                    name, d->base, d->clock_on ? "on" : "off", d->tx_pin ? "routed" : "unrouted",
                    control & UART_TX_ENABLE ? "on" : "off", control, divisor,
                    divisor ? d->reference_hz / divisor : 0, control & UART_TX_FORMAT_MASK ? " (unsupported format)" : "",
                    (unsigned long long)d->transmits, (unsigned long long)d->pin_transmits);
}

void sds_uart_set_clock(SdsUart *d, int enabled)
{
    uint64_t now;
    if (!d->nanoseconds || d->clock_on == enabled)
        return;
    now = *d->nanoseconds;
    sds_uart_advance(d);
    d->clock_on = enabled;
    if (!enabled && d->tx_due != UINT64_MAX) {
        d->paused_ns = d->tx_due - now;
        d->tx_due = UINT64_MAX;
        /* The shifter can resume, but a receiver at the pin lost this frame. */
        d->frame_visible = 0;
    } else if (enabled && d->paused_ns) {
        d->tx_due = now + d->paused_ns;
        d->paused_ns = 0;
    }
    start_frame(d, now);
    update_tx_request(d);
}

void sds_uart_set_tx_pin(SdsUart *d, int connected)
{
    if (!d->nanoseconds || d->tx_pin == connected)
        return;
    sds_uart_advance(d);
    d->tx_pin = connected;
    if (!connected)
        d->frame_visible = 0;
}

void sds_uart_cut_power(SdsUart *d)
{
    if (!d->nanoseconds)
        return;
    sds_uart_set_clock(d, 0);
    d->holding_full = d->frame_visible = 0;
    d->paused_ns = 0;
}

void sds_uart_reset(SdsUart *d)
{
    if (!d->attached)
        return;
    memset(d->regs, 0, sizeof(d->regs));
    d->rx = d->rx_ready = d->shift_byte = d->clock_on = d->holding_full = d->tx_request = d->frame_visible = 0;
    d->tx_due = UINT64_MAX;
    d->paused_ns = 0;
    d->tx_pin = 1;
}

static void timed_write(SdsUart *d, uint32_t offset, unsigned width, uint16_t old_holding)
{
    uint16_t control = d->regs[UART_TX_CONTROL / 2];
    if (!(control & UART_TX_ENABLE)) {
        d->holding_full = 0;
        d->tx_due = UINT64_MAX;
        d->paused_ns = 0;
    } else if (offset <= SDS_TX_DATA && offset + width > SDS_TX_DATA) {
        if (d->clock_on && !d->holding_full)
            d->holding_full = 1;
        else
            d->regs[SDS_TX_DATA / 2] = old_holding;
    }
    start_frame(d, *d->nanoseconds);
    update_tx_request(d);
}

static int range(SdsUart *d, uint32_t off, unsigned width)
{
    if (off + width > SDS_WINDOW) {
        bus_fault_access(d->bus, 0, d->base + off, width, 0, "SDS UART out of range");
        return 0;
    }
    return 1;
}

static uint32_t sds_read(void *ctx, uint32_t off, unsigned width)
{
    SdsUart *d = ctx;
    uint32_t value = 0;
    unsigned i;
    if (!range(d, off, width))
        return 0;
    if (d->nanoseconds)
        sds_uart_advance(d);
    if (off == SDS_RX_DATA && d->rx_ready) {
        uint32_t mask = width >= 4 ? 0xffffffffu : ((1u << (width * 8)) - 1u);
        d->rx_ready = 0;
        ++d->receives;
        return d->rx & mask & 0xffu;
    }
    for (i = 0; i < width; ++i) {
        unsigned reg = (off + i) / 2;
        uint16_t word = d->regs[reg];
        if (d->nanoseconds && reg == UART_TX_STATUS / 2)
            word = (uint16_t)((word & ~UART_TX_HOLDING_BUSY) | (d->holding_full ? UART_TX_HOLDING_BUSY : 0));
        value |= (uint32_t)((word >> (8 * ((off + i) & 1u))) & 0xffu) << (8 * i);
    }
    return value;
}

static void sds_write(void *ctx, uint32_t off, unsigned width, uint32_t value)
{
    SdsUart *d = ctx;
    unsigned i;
    uint16_t old_holding = d->regs[SDS_TX_DATA / 2];
    if (!range(d, off, width))
        return;
    if (d->nanoseconds) {
        sds_uart_advance(d);
        old_holding = d->regs[SDS_TX_DATA / 2];
    }
    for (i = 0; i < width; ++i) {
        uint16_t *word = &d->regs[(off + i) >> 1];
        unsigned shift = 8 * ((off + i) & 1u);
        *word = (uint16_t)((*word & ~(0xffu << shift)) | (((value >> (8 * i)) & 0xffu) << shift));
    }
    if (d->nanoseconds) {
        timed_write(d, off, width, old_holding);
        return;
    }
    if (off == SDS_TX_DATA && (d->regs[0] & 1u)) {
        /* A byte was loaded into the holding register of an enabled port: it is
           transmitted (with a virtual service cable, looped straight back into the
           receiver) and the register becomes free again, which is the TX service
           condition. The DMA controller loads the register with a byte write, the
           guest's serial services with a halfword write. Open time zeroes the
           register while the port is still disabled, so the enable bit gates it. */
        ++d->transmits;
        if (d->tx_log)
            fputc((int)(value & 0xffu), d->tx_log);
        if (d->cable) {
            d->rx = value & 0xffu;
            d->rx_ready = 1;
            d->intc->pending |= SDS_IRQ_RX;
        }
        d->intc->pending |= SDS_IRQ_TX;
    }
    if ((off == 0x00u || off == 0x08u) && (value & 3u)) {
        /* Control enable/arm: the TX register is empty, so the firmware TX
           service may load a queued byte. */
        d->intc->pending |= SDS_IRQ_TX;
    }
}

int sds_uart_attach(SdsUart *d, Bus *bus, Intc *intc, uint32_t base, int cable)
{
    memset(d, 0, sizeof(*d));
    d->bus = bus;
    d->intc = intc;
    d->base = base;
    d->cable = cable;
    if (!bus_device(bus, base, SDS_WINDOW, sds_read, sds_write, d))
        return 0;
    d->attached = 1;
    return 1;
}

int sds_uart_attach_timed(SdsUart *d, Bus *bus, Intc *intc, uint32_t base, const uint64_t *nanoseconds,
                          uint32_t reference_hz, int debug_port)
{
    if (d->attached || !intc->attached || (base & 1) || !nanoseconds || !reference_hz)
        return 0;
    if (!bus_device(bus, base, UART_WINDOW, sds_read, sds_write, d))
        return 0;
    memset(d, 0, sizeof(*d));
    d->bus = bus;
    d->intc = intc;
    d->base = base;
    d->nanoseconds = nanoseconds;
    d->reference_hz = reference_hz;
    d->tx_irq = debug_port ? DEBUG_IRQ_TX : SDS_IRQ_TX;
    d->tx_due = UINT64_MAX;
    d->tx_pin = 1;
    d->attached = 1;
    return 1;
}

int sds_uart_rx_ready(const SdsUart *d)
{
    return d && d->rx_ready;
}
