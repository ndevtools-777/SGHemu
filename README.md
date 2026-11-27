# SGHemu

Samsung SGH feature-phone emulator, starting with SGH-C100 / Skyworks CX805.
The shared core and tools are **C17**. The Windows build uses MSVC **2026 x86**
with `/W4 /WX`; Release uses `/O2 /GL /LTCG /MT` and Debug enables `/RTC1`.
The Linux GUI uses **C++17** and **Qt 5 Widgets** with the same C core.
The GUI also uses bounded execution and direct RAM access fast paths, without
idle-loop skipping. [Measurements](docs/bounded-execution.md) and an optional
[profile-guided build workflow](docs/profile-guided-build.md) are available.
Windows 7 API/subsystem target; actual execution on Windows 7 is untested.

[Download Windows x86 builds](https://github.com/drel4/SGHemu/releases).

> **Firmware compatibility:** **C100XEDK1** remains the best-tested version.
> **C100XEDA2 / C100XEDD1** now also enable the shared battery/charger, GPIO LED,
> low-power timer and firmware-matched cellular/SMS/tone bindings. WAP and import
> bindings are present but not equally tested across versions; see the
> [compatibility notes](docs/c100-firmware-compatibility.md). Other SGH models
> are not supported yet.

## Phone output and appearance

The status bar shows **Standby / Running / Paused / Stopped** and emulation speed.
**100% means real-time speed**, measured from emulated time versus wall time;
the display averages actual progress over about two seconds and reads zero while paused.

C100 navigation/key tones come from CPU/DSP emulation and are mixed with MA-3
audio when enabled. The program icon shows the native RGB service-LED color;
Qt also shows it in the status bar.
**L&F -> LCD ghosting** toggles a saved LCD response effect;
it does not alter guest pixels or screenshots. See
[implementation and verification](docs/c100-phone-output.md).

**L&F -> C100 MA detune (-12.5 cents)** optionally lowers Yamaha output pitch
to approximate the reported C100 detuning. It is off by default and saved;
CPU navigation tones are unchanged. This clock-style effect also lengthens MA
playback by about 0.72%; the exact hardware offset is not independently verified.

**L&F -> Water (not accurate)** enables a deliberately fictional shorted-pin MA effect: unstable
sequencer timing, voice/sample-data corruption bursts, bit-crushed/sample-held audio, dropouts
and crackles. It affects Yamaha sound, not CPU navigation tones. It is off by
default and session-only; a phone reset keeps the session's toggle. Turning it
off restores normal timing/output processing, but reselect the ringtone or reset
to clear corrupted queued pitch commands or uploaded samples. No firmware,
saved phone data, packet addresses or packet lengths are corrupted.

**File -> Logging** toggles `build/gui-unmapped.log`,
`build/gui-ma3-traffic.log` and `build/gui-profile.log` while running, without resetting the phone.
It also appends raw TX bytes for mapped serial ports to `build/gui-debug-uart.bin`
and `build/gui-sds-uart.bin`. Diagnostics show the last vectored ARM fault and,
for timed UARTs, the clock gate, GPIO route, divisor and bytes sent at the pin.
Logging is off by default, saved under `[diagnostics] logging` in
`state/emulator.ini`, and appends across toggles/resets. The old audio-only
`[audio] trace` setting no longer enables GUI logging. Performance profiling is
also off when logging is off; see [reading performance reports](docs/performance-profiling.md).

## AI-assisted project

This project is AI-assisted. If you hate AI-assisted software, don't use it.
This notice is not an additional restriction on the WTFPL license.

## Current milestone

The C100 firmware now reaches **normal standby with a virtual SIM**, showing
wallpaper and the Menu / Name soft keys. XEDK1 also has an experimental virtual
carrier for calls and SMS; other profiles still show **No Service**.
Pixels come from the guest's x16 LCD writes, not an extracted asset or host UI.
C100XEDA2, C100XEDD1 and C100XEDK1 have all passed SIM initialization and
standby captures. This is experimental hardware bring-up, not a complete phone:
unknown MMIO still continues and overall timing remains provisional, although
the clock tree uses firmware-derived clock modes. The native MA-3 core renders
guest startup and ringtone audio. The guest keypad matrix is connected. The
default strict profile still stops on unknown hardware; folder import selects
the documented bring-up profile.

Implemented: shared ARM7TDMI ARM/Thumb interpreter ([optimization and checks](docs/arm7-optimization.md)), ROM/RAM/config loader,
flash command/status handling, interrupt routing with a diagnostic frame source,
LCD command/data ports, and the observed PMIC serial-pin protocol. See
[LCD/boot evidence](docs/c100-lcd.md) and [memory map](docs/c100-memory-map.md).
The C100XEDK1 profile now uses GPIO registers for charger detection and service
LED output, plus an ARM/DSP ADC mailbox subset for battery measurements; these
paths no longer depend on firmware function addresses. Native firmware handles
battery conversion/filtering and charging animation. This is not a full DSP/RF
implementation. Navigation tones, virtual cellular/WAP and content-import helpers
still have firmware-specific bindings. See [battery support](docs/c100-battery.md)
and [ADC mailbox](docs/cx805-adc-mailbox.md).
The bring-up frontend now provisions missing NVM using the firmware's own
factory-sector formatter and initialization-marker writer, saves it, and cold
resets. Normal startup then creates defaults and initializes the virtual SIM.
Removing the virtual card restores **Insert SIM Card**. No physical GSM radio is
emulated; the XEDK1 virtual carrier can provide local service, while unbound
profiles still show **No Service**. See [SIM implementation and evidence](docs/c100-sim.md).
Existing saves load automatically without formatting. This uses a narrowly
verified internal-ROM version query, not fabricated BIST results. See
[internal-ROM analysis and UI evidence](docs/c100-irom.md).
The post-splash NVM startup failure and IRQ/flash fixes are recorded in
[startup tracing](docs/c100-post-splash.md).
The [persistent-storage investigation](docs/c100-nvm.md) distinguishes UI
records, blank-flash initialization and the reported 300 KB user quota.

## Build and run

On Windows, use an x86 Visual Studio Developer Command Prompt with native
Windows GNU Make on `PATH` (for example, MSYS2's `mingw32-make.exe`).

```powershell
.\build.cmd release
.\build\sghemu-gui.exe
```

`build.cmd` invokes the Makefile and keeps the original `build/` output paths.
Run `mingw32-make` directly to build the CLI and GUI under `build/windows/`.

On Linux, install GNU Make, a C17 compiler, `pkg-config`, and the ALSA and libcurl
development files (`libasound2-dev` and `libcurl4-openssl-dev` on Debian/Ubuntu;
`alsa-lib` and `curl` on Arch). Run from the repository root. The CLI and checks
do not require Qt, and the checks use synthetic data without firmware files.

```sh
make
make check
```

To try C100XEDK1, point `--folder` at the directory containing both ROM images:

```sh
./build/linux/sghemu --folder "/path/to/firmware" 10000000 --no-persist --lcd-out build/lcd.bmp
```

The CLI matches the images to a model by SHA-256 and saves the LCD frame to
`build/lcd.bmp`. `--no-persist` skips loading and saving phone state.

To build the Linux GUI, install a C++17 compiler, `pkg-config`, and the Qt 5
Widgets development files (`qtbase5-dev` on Debian/Ubuntu or `qt5-base` on Arch):

```sh
make gui
./build/linux/sghemu-gui
```

In **File > Open firmware folder (Ctrl+O)**, select the folder containing both
ROM files. If several pairs match, choose a version. The phone starts in
**Standby**; hold **End / Power (F4)** through startup. **Open model config** loads a
`.cfg`; the two advanced ROM actions can override its first and second images.
RAM-loaded and basic diagnostic profiles start stopped; press **Run (F5)**.
**Pause (F6)**
keeps the current LCD frame, **Stop / Reset (Ctrl+R)** cold-resets the phone and
leaves it stopped, and **Step instruction (F10)** advances a paused machine.
Use the on-screen keypad, or arrows, Enter/Space, F1–F4 (soft keys, Call/End),
digits, `*`, `#`, and Backspace. Phone state is saved in `state/` next to the
model config, as with the CLI.

The Qt GUI includes the phone filesystem and content queue, NVM backup/import,
cellular controls, diagnostics, and live audio. Its picture importer accepts
PNG/JPEG/GIF/BMP via Qt; the Linux CLI decodes uncompressed 24- and 32-bit BMP.

`make check` runs the synthetic tests; `make tests` also builds the optional
ROM tests, and `make tools` builds the available command-line tools. The
Makefile defaults to release; use `CONFIG=debug` for a debug build.

Release ZIPs contain the GUI, CLI, firmware-inspection and audio tools, model
configs, and documentation. Extract the whole archive and launch
`build/sghemu-gui.exe`; keep the supplied directory layout. Supply your own
firmware. See each release's notes for its audio requirements.

In the Windows GUI use **File > Open firmware folder (Ctrl+O)**. Pick the folder containing
both firmware files, or a parent folder containing several sets. Exact SHA-256
hashes identify the model/version even when files have been renamed. A recognized
pair starts in **Standby**; multiple matches offer a version selection. Hold
**End / Power (F4)** through startup. `.lst` files alone cannot boot.

The native window contains only the LCD, native keypad buttons beneath it, and
the top menu. F5 runs, F6 pauses, F10 steps, Ctrl+R resets (paused). State and
hardware counters are in View > Diagnostics. Buttons drive the firmware-scanned
keypad matrix. See [GUI controls](docs/native-gui.md) and [keypad mapping](docs/c100-keypad.md).
**Emulation > Keyboard text input (abc/ABC)**, or **Ctrl+T**, lets you type English
letters, spaces and periods directly while a phone text field is active. Select
the phone's English **abc/ABC** multi-tap mode (not T9) first; case follows the
phone's selected mode. Each character generates timed numeric keypad taps followed
by Right, so consecutive letters on the same key do not require waiting. Numbers
remain direct phone keypad presses. Backspace deletes; Esc cancels queued typing.
Queued text is discarded on focus loss, pause or reset. The toggle is saved;
turn it off to restore Space as the phone's OK button.
**Emulation > Virtual SIM inserted (reset)** toggles the card with a cold reset.
Its synthetic identity is configured in `models/c100/sim/test-sim.cfg`.
NVM automatically loads/saves under ignored `models/<model>/state/`, separated by firmware
hashes and memory map. Valid legacy `state/` saves migrate automatically without deleting the originals.
No manual Save command is needed. First boot takes
longer while the guest formats storage, then resets and boots normally.
File > Import NVM snapshot and reset remains for importing existing `.snv`
files. Use **Power** from Standby or **F5** from Stopped. ROM files are never modified.

Firmware belongs in model-specific, ignored folders:

```text
models/c100/roms/C100XEDA2/C100XEDA2_1.bin
models/c100/roms/C100XEDA2/C100XEDA2_2.bin
models/c100/roms/C100XEDD1/...
models/c100/roms/C100XEDK1/...
```

Imported files may remain in their original folder; selecting it does not copy or
modify them. Relative ROM paths in configs are **config-directory relative**.
See [profiles and folder identification](docs/firmware-layout.md) for new models.

```powershell
.\build\sghemu.exe --folder models/c100/roms/C100XEDA2 10000000 --lcd-out build/c100-lcd.bmp
.\build\fwinspect.exe models/c100/roms/C100XEDA2/C100XEDA2_1.bin --strings
.\build\sghemu.exe models/c100/c100xeda2.cfg 100000 --trace
```

CLI with several detected versions selects the first and reports that choice.
`--trace`, `--mmio-trace`, `--until ADDRESS`, `--continue-unmapped` are available.
CLI persistence is on by default; `--state-dir DIRECTORY` selects a different
store, and `--no-persist` gives an isolated erased-storage diagnostic run.
`--nv-load`/`--nv-save` remain explicit diagnostic import/export options.
`--sim present|none` overrides card presence for that run.
Diagnostic logs and captures go into ignored `build/`. GUI logging is disabled
by default and appends when enabled; CLI diagnostic runs use their own logs. Flash changes affect host-owned emulated memory only, not ROM files.

## Verification

`build.cmd release` runs synthetic and differential checks covering ARM/Thumb, memory and
peripherals, firmware detection, NVM, keypad/SIM, MA-3, content/JAR imports,
RTC, CPU tone/LED mixing, GPIO/ADC mailbox, decode-cache equivalence, peripheral
deadline scheduling and ADPCM lookahead. Synthetic tests require no
proprietary inputs. Optional real-ROM
regressions, compiled by the same build:

```powershell
.\build\c100_boot_test.exe models/c100/c100xeda2-bringup.cfg
.\build\c100_boot_test.exe models/c100/c100xedd1-bringup.cfg
.\build\c100_boot_test.exe models/c100/c100xedk1-bringup.cfg
.\build\c100_identity_test.exe
```

## Obtaining firmware

You can find firmware [here](https://t.me/samsfunclub/6/2543).
Download and extract the ROM files yourself, then choose
**File -> Open firmware folder...** and select the folder containing both BIN
images. The emulator detects supported C100 firmware sets by their hashes.
ROMs are not included in SGHemu releases.

## MA audio

Windows and Linux share the native Yamaha MA-3 core. Startup tones and music
play through WaveOut on Windows and the default ALSA device on Linux, without
Yamaha DLLs, Wine or a separate sound bank. **Emulation > MA-3 audio (reset)**
toggles speaker output and leaves the phone stopped; press F5 to resume. It
defaults to on and remembers the setting in `state/audio.ini`. Key tones remain
available with MA-3 output muted.

Use `--audio` for speakers or `--audio-wav FILE.wav` for 48 kHz stereo capture.
The options can be combined. `--audio-ticks-hz N` applies only to model profiles
without a PLL clock; it specifies interpreter ticks/sec. The deprecated
`--ma5-hw PATH` compatibility option ignores the path.

```sh
./build/linux/sghemu MODEL.cfg 80000000 --audio-wav build/startup.wav
```

The core consumes the guest's byte-port FIFO packets, renders FM and PCM/ADPCM
voices, and advances timers, IRQs and stream cursors in emulated time even while
muted. The built-in instrument presets approximate the chip's ROM voices;
some modulation and timing behavior still differs from the reference.

### Audio diagnostics

`make tools` builds `ma3pcmprobe [OUTPUT.wav]` to check native stream timing,
pitch and refill, and `ma3replay TRACE.log OUTPUT.wav [TICKS_HZ]` to replay
MA-3 port traffic. Use a trace with a consistent timebase for timing comparisons.
The earlier MA-5 comparisons are recorded in the [historical notes](docs/ma5-audio.md).

## Phone filesystem and services

The phone filesystem manager supports directory inspection, TSV export, NVM
backup and content import on C100 XEDK1. **File -> Phone filesystem manager...**
opens a two-pane manager: host import
queue on the left, phone files on the right. **Add files...** accepts sounds, PNG/JPEG/GIF/BMP pictures and
CLDC 1.0 / MIDP 1.0 JARs. Pictures are fitted to 128 x 128 (first frame only).
Use **Import queue >>** for bulk loading; each item is saved before the next
starts. For a JAR, wait for the phone's **Installed** screen, then press the
phone's **Close** softkey. **Java games** reboots into the native MIDlet menu
using the saved NVM. These installer entries are specific to XEDK1.
The handset's own Java menu is WAP & Games -> Downloads -> Games and more.
For larger apps, enable **Emulation -> Extended Java buffers (next import/reset)**
before importing again. This optional compatibility mode enlarges the native
installer's working buffers to 1 MiB each; this is **not original hardware behavior**.
It is off by default and does not replace the native installer or VM. Compilation
can take minutes. A MIDP 1.0 manifest alone does not guarantee an app will run.
The **Emulation** menu also provides **High process priority** (off by default)
and **Sync phone date/time to host** (on by default for C100 bring-up profiles).
Priority changes take effect immediately when the host permits them.
Host sync uses local time, including the timezone/DST settings. Switching
sync off allows handset clock changes and advances the RTC with emulated time.
These preferences persist in `state/emulator.ini`; phone NVM stays model-local.
CLI equivalents: `--high-priority` and `--rtc-sync` (both opt-in on the CLI).

**Emulation -> Cellular** (XEDK1) provides an enabled toggle, a custom network
name (default **Tele2**), **Send SMS to phone...** and **Call phone...**. Dialing
from the handset opens a host **Incoming call from phone** window with
**Accept / Decline**, then **Hang up** when connected. These are local simulated
calls/messages, not connections to a real operator; voice-call audio is not
implemented. Incoming SMS can be read and replied to in the native phone menus.
SIM contacts, SMS and message parameters are automatically saved beside each
firmware's NVM as `.snv.sim` and restored on reset/startup.

**Cellular -> GSM** selects **Off**, **Internet**, or **Internet + built-in WAP
gateway** (packet data only). Selecting the built-in gateway shows setup
instructions: gateway IP **10.0.0.1**, non-secure WAP, port **9201**; test homepage
**http://sghemu.local/**. Configure these in the phone's WAP browser profile.
The gateway fetches HTTP/HTTPS through WinHTTP on Windows or libcurl on Linux,
with a small text/link conversion for old WAP browsers. Java has a separate
asynchronous DNS/TCP bridge for native HTTP downloads (including JAD-to-JAR
installation) in both internet modes on XEDA2/XEDD1/XEDK1. This is not complete
GSM, Java UDP, WTLS or a full HTML browser. See
[Java networking](docs/java-network.md) and
[cellular and SIM details](docs/c100-cellular.md).

The GUI reopens the last firmware in **Standby** if the profile supports a
cold boot. In Standby, CPU, LCD and UART TX are off while the RTC advances.
Power, charger insertion or a programmed RTC alarm cold-boots with saved NVM
and the session's SIM, clock and host settings. Pressing Power feeds the board
immediately; releasing before the firmware sets HOLD returns it to Standby.
The firmware controls the LCD and its backlight. **Emulation -> Battery** offers
an editable 0-100% level and a charging-cable toggle, persisted in
`state/emulator.ini`. These drive ADC/GPIO inputs on XEDA2/XEDD1/XEDK1; the phone
samples and filters them, so its battery bars update later. Connect the cable
from Standby or shut down with the cable attached to see the charging screen.
At 100%, the charger signals completion after 20 emulated seconds, so the
firmware can hatch the egg. The voltage curve and charger delay are provisional;
see [battery details](docs/c100-battery.md).
See [content import details](docs/c100-content-import.md).

CLI Java examples (use your own game file):

```powershell
.\build\sghemu.exe models/c100/c100xedk1-bringup.cfg --import GAME.jar --lcd-out build/installed.bmp
.\build\sghemu.exe models/c100/c100xedk1-bringup.cfg --java-menu --lcd-out build/java-menu.bmp
.\build\jarinspect.exe GAME.jar
```

The manifest check is not an API-compatibility guarantee. Games requiring
MIDP 2.x or APIs absent from the original handset may not run. The JAR manifest
decoder uses Mark Adler's [puff](third_party/puff/README.md), with its original
license preserved separately from the project's WTFPL.

## Layout

| Directory | Contents |
|---|---|
| `src/` | Shared CPU, bus, peripherals, native GUI, firmware loader, audio interface |
| `models/` | Model memory/peripheral configs and hash-identification descriptors |
| `tools/` | C firmware/audio tools and Ghidra Java analysis scripts |
| `tests/` | Instruction programs, protocol fixtures, optional real-ROM regressions |
| `docs/` | Reverse-engineering evidence and remaining hardware work |
| `models/**/roms/`, `build/` | Ignored private inputs and build artifacts |

The CLI (`src/main.c`), Windows GUI (`src/gui.c`) and Linux GUI (`src/gui_qt.cpp`)
share `src/machine.c`, which loads the model and runs the emulated phone. Host
file and clock operations are declared in `src/port.h` and implemented in
`src/port_posix.c` or `src/port_win32.c`, selected by the Makefile.

## License

Copyright (c) 2026 drel4 and SGHemu contributors. SGHemu's original source code,
tools and binaries are licensed under the [WTFPL version 2](LICENSE).
This license does not apply to Samsung firmware, Yamaha libraries/voice banks,
or other third-party material, which is not included in releases.
