# C100 LCD and first visible boot

## Reproduction

Build, open `build/sghemu-gui.exe`, choose File > Open firmware folder, and select
a complete supported C100 ROM pair. Alternatively:

```powershell
.\build\sghemu.exe --folder models/c100/roms/C100XEDA2 10000000 --lcd-out build/c100-lcd.bmp
```

At 10,000,000 instructions the guest has written 81,665 pixels and displays its
128x128 **GPRS splash**. XEDA2, XEDD1 and XEDK1 were checked separately.
BMP snapshots encode the visible framebuffer, including display-off/standby
blanking. No image extraction, firmware patch, forced PC jump or drawing hook
is used. Generated captures remain ignored private artifacts.

## Evidence and controller mapping

The newly supplied XEDK1 linker listing places its second image in LOAD2/RDATA
at `0x02000000`; the image objects reside there. This resolves the earlier
unplaced-ROM hypothesis. Both flash windows are mapped to 8 MiB and their tails
are initially erased (`FF`); exact physical flash geometry/IDs are unconfirmed.
RAM end is `0x007E05C0` for XEDA2 and `0x007E0600` for XEDD1/XEDK1, verified
against the bootstrap literal at file offset `0x6A0` and the XEDK1 linker map.

The listing names `lcd16_write`, `lcd16_DecInit`, `lcd16_InvalidateAll` and the
LCD power helpers. Matching instruction bytes to XEDA2 gives:

| Function | XEDK1 | XEDA2 |
|---|---:|---:|
| lcd16_write | `011B9E5A` | `011B95AE` |
| lcd16_DecInit | `011B9F8A` | `011B96DE` |
| lcd16_InvalidateAll | `011396FC` | `01139720` |
| lcd16DisplayOn | `01139D32` | `01139D56` |

RAM pointer `00407AD8` contains `00C00000`. `lcd16_write(0,value)` uses this
command port; `lcd16_write(1,value)` adds `00080000` for pixels (`00C80000`).
Commands, parameters and pixels are all halfword writes. Command `43` sets
controller Y from 16 to 143; `42` sets X from 0 to 127. Entry mode `40 00`
increments Y first. Host column is controller Y minus 16; host row is X.
Pixels are RGB565. The fast pixel writer is firmware-copied code in low RAM.

The observed command family matches Samsung S6B33 controllers. The 144-column
S6B33B0A is a plausible part; the exact fitted controller is **not confirmed**.
The generic implementation uses a 256x256 addressable backing store and a
configured viewport, not a claim about the physical chip capacity.

Primary authored references: Samsung [S6B33B0A v1.1](https://www.displayfuture.com/Display/datasheet/controller/S6B33B0A.pdf)
and [S6B33B2 v1.2](https://www.displayfuture.com/Display/datasheet/controller/S6B33B2.pdf).
The display resets off. The detailed Display On section requires standby to be
cleared before `51` can enable output. Status returns DP bit 0,
REV bit 1, STB bit 2 and X/Y bit 6, without a busy delay. Most analog/power
parameters are consumed without simulating the circuit. Burst On, partial
display and scrolling are not implemented. Burst Off (`2E`) is acknowledged
for the observed x16 path; unknown commands are logged.

## Backlight, contrast and PWM

On [the C100 schematic][c100-lights], A7 (`LCD_BACKLIGHT_EN`) enables the
LT1937 LED driver. C4 (`LCD_BACKLIGHT_CONTROL`) switches the additional current
sense path through Q400/R403, making the backlight brighter. The lower current
uses R402. A4 controls the keypad light separately. [FreeCoder's C100 LCD
driver][freecoder-lcd] selects three states: A7 low = off, A7 high/C4 low =
normal, A7 high/C4 high = bright. XEDK1 `uhBacklight` (`0x0113DF40`) writes
those GPIOs too. The output latches only drive the lights in GPIO output mode.

`lcd16SetContrast` (`0x0113A066`) sends `2A` followed by one byte. The value
changes LCD drive voltage independently of the backlight driver. The renderer
uses `A8` (FreeCoder's setup) as neutral contrast. `2B` is for partial display
mode 1 and has no effect in normal mode. Contrast and light changes repaint
the existing DDRAM in both GUIs and `--lcd-out` BMPs. The unlit panel remains
faintly visible; display-off, standby and board power-off blank the image.
The displayed brightness and contrast curves are approximations pending
measurements of the handset panel.

C4 mux value `01` selects the [CX805-30 annunciator PWM][cx805]. The two
generators at `0x00040A00` and `0x00040A10` each have control, duty, divider,
count and 32-bit pattern registers. The 1.95 MHz reference advances rotating
pattern readback. SGHemu averages the two interleaved outputs to set the
increased-current fraction of the backlight while A7 is high. This is a display
approximation; LT1937 switching and panel luminance have not been measured.
In the XEDK1 boot trace through 320M instructions, both writes to PWM control
at `0x00040A00` were zero. Its lighting changes used GPIO.

A7 mux value `01` selects the Timer 2 pulse output. SGHemu reports this selection
and suppresses the GPIO latch; it leaves the panel unlit in that mode pending
evidence for pulse width and polarity. Both frontends and the CLI report the
selected light mode, contrast and keypad light state.

## Why the first attempt stayed black

The firmware reached LCD initialization and wrote colored DDRAM, but later
powered down: `50`, `2D`, descending `26` parameters and oscillator `02 00`.
A delayed `51` then arrived without `2C`. Displaying DDRAM despite standby
would have hidden a boot-flow bug.

The PMIC driver bit-bangs serial pins at `00040300` (SDA) and `00040302` (SCL).
Bit 0 drives each output; bit 1 reads the actual line. `IICShiftByteOut` verifies
SDA echo before each clock and checks an acknowledgement after eight bits.
Returning zero for every read made all transmitted one bits fail.

`pmic.c` now handles START/STOP, MSB-first transmission, ACK/NAK, register index,
reads and writes. Observed PMIC byte address is `90/91`; read transactions send
`91`, then the index, then receive bytes without another START. Register 8 is
used by the power-cause path. Register defaults are **provisional zeros**, not a
complete documented PMIC. The firmware itself interprets this initial status
as power reason 1 and follows the visible boot path. No guest RAM reason is
injected. A regression using `A5` and `3C` read bytes catches clock-edge errors.

Active-low keypad input `40804` is held idle at `FFFF`, and charger GPIO
`4028E` bit 2 is high (disconnected). These are diagnostic input fixtures;
key matrix scanning and dynamic charger/battery emulation remain unfinished.

## Remaining provisional behavior

- Firmware-accepted class-3 identity tuple, actual C100 revision unconfirmed.
- Cold-boot handoff `40A80=20017420`, inferred from `DoStartupCheck`, not verified
  against hardware. Without it the image invokes its fatal reboot path.
- IRQ controller `40100`: pending/W1C at +0, enable +8, FIQ selection +C,
  filtered FIQ +14 and IRQ +18. Source bit 26 enters `FrameTickIRQHandler`
  (XEDA2 `0114D55A`), which calls `KS_ISRtick(1)` at `01389B34`.
- Diagnostic source period: 10,000 interpreter ticks, instruction-driven.
  This is not a measured GSM period or a CPU frequency estimate.
- Intel/Sharp x16 flash status/program/erase commands observed in RAM driver.
  Operations complete immediately. Erase uses 64 KiB regular blocks and
  model-configured 8 KiB boot blocks (bottom of bank 0, top of bank 1),
  matching all three firmware sector tables;
  lock/partition commands are accepted without full protection geometry.
- Unknown MMIO continues with zero reads/dropped writes and an access log.
- Visible splash is not yet a fully usable phone UI. Guest keypad, SIM, DSP,
  ADC/battery, calibrated timers and MA-3 audio still require work.

Post-splash tracing and current NVM failure: [C100 startup after the splash](c100-post-splash.md).

[c100-lights]: https://github.com/Krasutski/SamsungC100_firmware_handmade/blob/b87b59a/doc/sch/shem_led_keyb_LCD_spek_vibr.pdf
[freecoder-lcd]: https://cx805.narod.ru/LCD.htm
[cx805]: https://github.com/Krasutski/SamsungC100_firmware_handmade/blob/b87b59a/doc/datasheets/CX805-30.pdf
