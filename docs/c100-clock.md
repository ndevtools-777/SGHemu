# C100 clock evidence

Analyzed with Ghidra 12.1.2 on 2026-10-10 using the supplied XEDK1 firmware
and linker listing. These are firmware-reported clock rates, not measurements
of a physical handset. Addresses below apply to XEDK1 only.

## ARM and DSP are different clocks

`GetARMClockFrequency` at `0x0115E62E` and `GetDSPClockFrequency` at
`0x0115E656` read `gCurrentMode` at `0x004078B8` and return literal Hz values:

| PLL mode | ARM clock | DSP clock | ARM literal | DSP literal |
|---|---:|---:|---|---|
| 0, or unknown mode | 3,900,000 Hz | 3,900,000 Hz | `0x0115E6A0` | `0x0115E6A0` |
| 1 | 19,500,000 Hz | 39,000,000 Hz | `0x0115E6A4` | `0x0115E6B0` |
| 2 | **23,400,000 Hz** | **46,800,000 Hz** | `0x0115E6A8` | `0x0115E6B4` |
| 3 | 25,350,000 Hz | 50,700,000 Hz | `0x0115E6AC` | `0x0115E6B8` |

Ghidra instruction listings confirm the comparisons against 0/1/2/3 and the
literal loads; the values above were also read directly from the original ROM
at file offset `literal_address - 0x01000000`. No PLL formula had to be guessed.

`DoBootInitialization` at `0x0111F3E4` calls `PLLSetMode(0)` and then
`PLLSetMode(2)` (confirmed BLs at `0x0111F406` and `0x0111F40C`).
`HWServiceWAKEUP` at `0x01151068` calls `PLLSetMode(2)` at `0x0115106C`.
`HWSleep` at `0x01150B96` can select mode 0. Thus the recovered awake/boot
setting is **ARM 23.4 MHz, DSP 46.8 MHz**, not an ARM clock of 46.8 MHz.

A read-only local interpreter probe ran 35 million instructions with an
initialized XEDK1 NVM fixture, without saving storage or changing firmware.
It read `gCurrentMode=2` and the matching ARM literal `23400000`. The guest
PLL dispatch pointers were `0x01218B91`, `0x01218BE9`, `0x01218C3D`, matching
the Thumb entry points of the 80500_11 39/46.8/50.7 MHz setters. This probe
validates the selected mode/dispatch, not real-time accuracy or handset identity.

## Recovered register accesses

- `0x00040B00`: PLL frequency programming register. Revision-specific setters
  write/read/write this halfword; encodings differ between revisions.
- `0x00040B02`: second PLL programming halfword. Literal values for the
  39/46.8/50.7 MHz setters are respectively `0x0901`, `0x0B01`, `0x0C01`.
- `0x00040B04`: `EnablePLL` sets/clears bit 3. `SetClockSourceHigh` sets/clears
  bits 0..1 with its first argument and bit 2 with its second argument.
- `0x00040902` and `0x00040906`: the 805 clock-control driver applies
  set/clear masks; writes to the latter are masked to 11 bits. These are not
  evidence that its bit numbers encode CPU MHz.

For the dispatched 80500_11 46.8 MHz PLL setter at `0x01218BE8`, the first
halfword is `0x000B`, the second `0x0B01`. The 80500_31 variant uses `0x002B`
for the first halfword. Do not collapse revision-specific encodings into a
single assumed multiplier formula.

## What this changes in the emulator

The C100 profiles now map the PLL registers and use the selected ARM rate to
advance emulated nanoseconds. `Arm7.clock_cycles` estimates instruction timing
with S=N=I=1, including loads/stores, multiplies, branches and pipeline refills.
External memory wait states and bus widths are not modeled. Atomic internal-ROM
HLE calls have provisional minimum timing. This is not cycle-accurate hardware.
The original `Arm7.cycles` remains an instruction/exception scheduler counter;
legacy SIM/DMA timing still uses it.

MA-3 synthesis now advances from emulated nanoseconds at a 1,000,000,000 tick/sec
timebase. IRQ 26 is scheduled every 4,615,385 ns, independently of instruction
count. It remains a diagnostic GSM-frame source, not a complete DSP timer model.
The legacy audio `ticks_hz` setting applies only to profiles without a PLL model.
XEDK1 boot with initialized storage reaches standby in a 250-million-instruction
capture: 525,656,788 estimated core cycles and 23.771928 seconds of guest time.
The last-boot startup sound is active for 3.808979 seconds in this offline capture;
that does not establish real-handset tempo or fix the reported game/selection speed.

The recovered `RTXCISRMultiFrameTick` also contains a 13-frame dithering counter;
that is OS timer bookkeeping, not proof of a CPU divider or PLL reference.

## Standalone timers, sleep and RTC alarm

The standalone profile maps 26-bit Timer 1 at `0x00041300` (IRQ 0) and Timer 2
at `0x00041400` (IRQ 1). They count down with separate period and count loads;
the control register at `+0x0C` selects stop, IRQ enable, split-half loading and
clock source. Interval timing uses the 3.9 MHz reference by default. Timer 2 can
instead count ARM cycles and is gated by clock-control bit 5. The physical Timer
1 rate at loader handoff, the SIM ETU/external timer inputs, and the width of the
Timer 2 pulse on A7 remain unverified.

In standalone profiles, `0x00040902` bit 2 requests sleep. The CPU's instruction
and core-cycle counters stop while emulated time advances. An enabled pending
IRQ or FIQ wakes the CPU; ARM CPSR masks then decide whether it takes an
exception. The low-power comparator at `0x00040700` uses 32,768 Hz independently
of the ARM clock and raises IRQ 25. Stock XEDK1 `HWSleep` programs that comparator,
and `HWServiceWAKEUP` restores PLL mode 2. The stock profiles retain their
existing IRQ-gate model at `0x00040902`.

The RTC at `0x00040080` compares minute through year at `+0x12..+0x1A` and
latches IRQ 2 on a match. XEDK1 disables the alarm by writing `FFFF` to the
minute compare; INTC pending is cleared by writing one at `0x00040100`.
`RTCServiceAlarm` also writes `1,1,2` to `+0x1C`, documented as external interrupt
control. The C100 schematics connect the [CX805 `ALARM` output (U300 C13)][cpu-sch]
to [CX20524 `ALARM_IN` (U200 J13)][pmu-sch]. The [CX20524-12 datasheet][pmu-spec]
specifies that pulling this input low initiates power-up. SGHemu uses the
comparator match to power on from Standby; `+0x1C` is retained as a latch while
its bit meanings and output timing remain unverified. The storage words at
`0x000400A0..0x000400BE` and the clock survive sleep and Standby in the same
session. The CLI can advance Standby with `--rtc-off-ms N` or inject a wake
input with `--wake-with power|charger` after the guest powers off.

`make check` exercises both timer clock domains, timed UART TX, ARM exceptions,
and comparator/RTC wake with single steps and batches. These regressions verify
guest-visible scheduling in SGHemu. They do not calibrate a handset.

## Reproduction

Use the existing `C100Nvm.java` Ghidra helper with the XEDK1 linker listing
and select `GetARMClockFrequency`, `GetDSPClockFrequency`, `PLLSetMode`,
`DoBootInitialization`, `HWServiceWAKEUP`, and `HWSleep`, with `depth:0`.
Select the `ds_SetPLLFrequency*` functions separately to inspect revision
encodings. The original ROMs, raw decompilation, probe and logs remain ignored
local artifacts under `models/c100/roms/` and `build/`.

[cpu-sch]: https://github.com/Krasutski/SamsungC100_firmware_handmade/blob/b87b59a/doc/sch/shem_cx80501-30.pdf
[pmu-sch]: https://github.com/Krasutski/SamsungC100_firmware_handmade/blob/b87b59a/doc/sch/shem_cx20524-12.pdf
[pmu-spec]: https://github.com/Krasutski/SamsungC100_firmware_handmade/blob/b87b59a/doc/datasheets/CX20524-12.pdf
