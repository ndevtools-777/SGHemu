# Firmware folders and model profiles

ROM sets live under `models/<model>/roms/<version>/`, which is gitignored.
Native GUI File > Open firmware folder selects an arbitrary existing folder;
ROMs need not be copied into the workspace. The scanner descends up to three
subfolder levels, ignores reparse points, and hashes at most 512 files no larger
than 32 MiB. File extension/basename is not used to assume a memory map.

Model descriptors `models/<model>/<version>.firmware` contain:

```ini
name=Phone and version
model=version-bringup.cfg
sha256_1=<64 hex digits>
sha256_2=<64 hex digits>
```

Both hashes must be present. Renamed images work; an incomplete pair, changed
image, unknown firmware or symbol listing alone does not boot. If several
complete profiles match, the GUI offers their names; the CLI reports all and
uses the first. Supported local C100 sets: XEDA2, XEDD1, XEDK1. XEDB2 currently
has only a symbol listing, not executable ROMs. New models require verified
maps/peripheral configuration and descriptors; importing an unknown folder
never guesses that another phone is a C100.

The XEDA2/XEDD1 profiles now share the GPIO/ADC battery and low-power timer
hardware with XEDK1 and provide their own relocated cellular, SMS, tone, import
and WAP service bindings. See [per-version verification and limits](c100-firmware-compatibility.md).

Model config paths are relative to the config file, not the process cwd.
Explicit import overrides are absolute. Configuration directives:

| Directive | Meaning |
|---|---|
| `name=text`, `reset=address` | Identity label and reset PC |
| `rom=base,path[,mapped_size]` | Load ROM; optional erased FF padding |
| `ram=base,size[,path]` | Writable RAM; optional raw load image at the base, zero-filled remainder. A RAM image can boot without a `rom` entry. |
| `alias=base,source,size` | Mirror a mapped memory region; overlaps rejected |
| `display=width,height`, `audio=silent` | Host display and audio backend |
| `flash=base,manufacturer,device` | x16 flash interface on a mapped ROM |
| `c100_nor_boot=1` | Model power-on at reset PC 0: require the BIN1 header, configure CS5/6 x16 waits and expose the provisional 32-byte vector alias after IROM handoff. |
| `nvram=base,size` | Optional snapshot window within a flash bank's padded tail; must not overlap supplied firmware bytes |
| `nv_provision=trigger_pc,format_pc,marker_pc` | First missing persistent save only: call verified Thumb guest factory formatter and marker writer at the initialized trigger, commit, cold reset; all entries must lie in supplied ROM |
| `flash_geometry=base,offset,size,erase_size` | Override erase size for a flash subrange; after `flash`, nonoverlapping, covering whole default 64 KiB sectors |
| `lcd_s6b33=command_port,data_port,origin_y` | Guest LCD interface/viewport |
| `c100_backlights=1` | Wire LCD, PWM and keypad lights in stock C100 profiles. Requires `lcd_s6b33` and `gpio_port`/`gpio_led` on A (`0x4028C`); adds C and the A/C direction and mux registers. |
| `c100_power_key_ms=milliseconds` | Enable C100 PMU power and set the automatic startup pulse and CLI-simulated hold duration. A real Power press starts the board immediately; releasing it before the guest asserts PMU `03` bit 0 (HOLD) removes power. Standalone profiles also get GPIO, LCD select, lights and keypad with IRQ 24. Requires `clock`, `lcd_s6b33` and `pmic`. |
| `pmic_wake_xor=mask` | Compatibility mask for PMU `08` at wake. Stock Sysol profiles use `0x07` for the active-low causes observed in XEDK1; standalone profiles use the CX20524 encoding by default. |
| `intc=base`, `periodic_irq=bit,period` | Interrupt routing and diagnostic tick source |
| `clock_control=base,initial` | Standalone clock enables and sleep request; requires a PLL `clock=base,reference_hz`. |
| `timer=base,number` | 26-bit standalone timer, number `1` or `2`; requires `clock` and `intc`. |
| `rtc=base,year_base` | Calendar, alarm compare (IRQ 2 with `intc`) and RTC storage words. |
| `lp_timer=base,hz,cal_irq,wake_irq,cal_period_ns` | Low-power oscillator, calibration tick and wake comparator. |
| `uart=base,debug\|sds` | Timed CX805 TX; CLI `--uart-out FILE [--uart-port debug\|sds]` and GUI **File → Logging** capture bytes that complete on a routed TX pin. |
| `sds_uart=base,cable` | CX805 UART; CLI `--uart-out FILE` captures transmitted bytes. Virtual loopback requires `--service` and `cable=1`. |
| `exceptions=arm7` | Deliver architectural undefined-instruction and bus-abort exceptions to the guest vectors. |
| `bus_abort=base,size` | Treat bus faults in this range as an external abort. Other unmapped accesses remain diagnostic faults. |
| `ma3=base,irq_mask,sequence_quantum_ns` | MA-3 byte ports and interrupt source; model-specific divider time unit, 1157 ns inferred from C100 SetSpeed |
| `pmic=base,byte_address`, `pmic_register=index,value` | Observed serial pins and provisional initial PMIC state |
| `identity16=address,value` | Explicit read-only diagnostic input fixture |
| `register32=address,value` | Explicit writable latch/handoff fixture |
| `irq_gate=address,mask` | IRQ delivery enable bits in a mapped `register32` halfword; requires `intc`, leaves pending sources latched |
| `irom_service=address,memcpy` | Explicit Thumb internal-ROM memcpy entry; must not overlap mapped memory |
| `irom_version=address,halfword,return_pc` | Caller-specific Thumb version query; writes only the verified first halfword, rejects other callers; `FFFF` means unknown in C100 bring-up profiles |
| `irom_vector=address,thumb_target` | Read-only 32-bit indirect vector in the internal-ROM window; target must name a configured HLE service. Selector 18 uses version/memcpy vectors at 80030/8003C; these alias existing HLE contracts, not dumped ROM bytes |
| `intc_aux=base` | Optional second interrupt bank sharing the CPU's IRQ/FIQ lines |
| `charger_complete=port,done_bit,enable_bit,bank,irq,settle_ms` | GPIO charger-completion subset; active-low enable, done held until unplug/SOC drop, bank 0 main or 1 auxiliary |
| `diagnostic=continue` | Bring-up profile allowing logged unknown MMIO |
| `unplaced_rom=path` | Legacy retained image without a guest mapping |

The shared CPU is independent of model configs. Peripheral register/pin
locations and initial states belong to model profiles. Private firmware/DLLs,
voice files, listings, flasher S-records and generated images are never committed.

## Cold NOR and the separate XEDK1 power-off demo

On a Power press from Standby, the PMU starts the baseband with a power-on
reset, as described in [FreeCoder's PMU notes](https://cx805.narod.ru/PMU.htm).
The key keeps startup powered until the firmware sets HOLD over I²C. Releasing
the key sooner cuts power even if the CPU is halted. Clearing HOLD after the
handoff can shut the board down while the key is still pressed; a new press
starts a fresh reset.

`c100_nor_boot=1` models the reset delay and initial NOR reads before entering
BIN1 at `0x01000038`. `fwinspect FILE --c100-nor` checks the same length at
`+0x40`, `CBCB` marker at `+0x44` and entry branch as the emulator. The vector
window and CS5/6 timing are provisional. The modeled IROM handoff does not run
the `0x81`/`0x7E` SDS loader exchange. A profile can instead start directly at the
NOR handoff, as the stock bring-up profiles do, or load a RAM image.

For the **GPRS splash → Standby** demo, copy both stock XEDK1 BIN files to a
separate folder. In the copy of BIN1, replace `8F 20` with `8E 20` at file offset
`0x0011EA18`. This clears PMU HOLD in the initial power setup. Open that folder
in the GUI; `models/c100/c100xedk1-broken.firmware` matches the two demo hashes.
Hold **End / Power** until the GPRS splash appears, then release it to return to Standby.
