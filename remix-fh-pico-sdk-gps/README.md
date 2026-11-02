# remix-fh-pico-sdk-gps — GPS blade (native Pico SDK, BENCH-PROVEN)

> Raspberry Pi **Pico (RP2040)** + **NEO-6M**, native **Raspberry Pi Pico C/C++
> SDK** (no Arduino layer). A polled SPI **slave** on the P4 hub's **Bus B**,
> speaking the binary **fh_protocol**. **This is the WORKING, bench-proven blade
> (2026-11-01):** MODE 1 261-byte stream, CRC-valid `GPS_FIX` frames, hub RTC
> locked from a blade frame, `PICO=UP`. It is the **reference implementation**
> every other Pico blade copies its SPI transport from.

**This blade's docs:** `WIRING.md` (hookup) · `README.md` (this — for people) ·
`AGENTS.md` (for AI/LLM agents — read before editing) · `CONTINUITY.md` (resume
point) · `FUTURE.md` (planning). Plus `PICO_LESSONS.md` — the hard-won RP2040
lessons, kept under its own name because the whole repo references it; read it
before debugging any Pico blade link problem.

## What it does
- core1 parses the NEO-6M NMEA (GGA + RMC, checksum-verified), stages the latest
  `fh_gps_fix_t` (16 B) at 5 Hz, and builds the outbound frame.
- core0 runs the SPI-slave link: 261-byte full-duplex transactions on spi0,
  decodes inbound frames (PING → PONG), and hands the staged fix to the hub on
  the next transaction.
- The hub POLLS this blade on `cfg.gps_rate` cadence; each poll returns a
  `FH_TYPE_GPS_FIX` (0x02) frame → the hub caches the fix and sets its RTC on the
  first plausible epoch.

## How it attaches
- **SPI slave ↔ hub Bus B:** spi0, SCK GP18 / MOSI GP16 / MISO GP19 / CS GP21,
  **MODE 1**. Full table + the NEO-6M uart1 wiring are in `WIRING.md`.
- No radio chip — the "payload" is the GPS module on uart1 (GP4/GP5).

## Build / flash (Windows / git-bash)
```bash
export PICO_SDK_PATH="C:/Users/Admin/pico-sdk"
export PICO_TOOLCHAIN_PATH="/c/Users/Admin/AppData/Local/Arduino15/packages/rp2040/tools/pqt-gcc/5.0.0-9576866"
export PATH="$PICO_TOOLCHAIN_PATH/bin:$PATH"
cd /c/Users/Admin/Desktop/FootHoled-NextGen/remix-fh-pico-sdk-gps
"C:/Program Files/CMake/bin/cmake.exe" -B build -S . \
  -DPICO_BOARD=pico -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -G Ninja
"C:/Program Files/CMake/bin/cmake.exe" --build build
python uf2conv.py build/fh_pico_gps.bin -o build/fh_pico_gps.uf2
```
This machine has no host C compiler, so `PICO_NO_PICOTOOL=1` is set and the
`.uf2` is made from the `.bin` with the bundled `uf2conv.py` (standard UF2:
512-byte blocks, RP2040 family 0xe48bff56). Flash: hold **BOOTSEL**, copy the
`.uf2` to the `RPI-RP2` drive via PowerShell `Copy-Item` + eject (a bare `cp`
does NOT flash). **5 quick boot blinks = new firmware running.** A pre-built
release lives under `releases/` (see `releases/README.md`).

Indoor bench with no sky view: add `-DFH_GPS_SIM=1` (never ship a field rig with
it). Optional debug UART: `-DFH_DEBUG_UART=1` (uart0 GP0/GP1 @115200).

`slave_min.c` is a SEPARATE bench target (`cmake --build build --target
slave_min`), not part of the blade image. It is the bare-metal SPI-slave
isolation test, and it runs **MODE 0 @ 1 MHz**. MODE 0 is the RP2040 SSP mode
that transmits only ONE byte per CS assertion (RP2040 datasheet / pico-examples
issue #115 — see the hub's `busB_init` note), which is WHY the production blade
(and every blade copied from it) runs **MODE 1**.
Do NOT copy its SPI format into a blade.

## Protocol (frozen; byte-identical `fh_frame.c/.h`)
- Two transactions per hub poll, each exactly 261 B (`FH_MAX_FRAME`). The first
  (hub request) is discarded — a polled periodic source always returns its latest
  fix; the second clocks out the staged frame, zero-padded (all-zero prime buffer
  if no fix). PING (0x7D) → PONG (0x7C) staged and wins the next build.

## Why native SDK (not Arduino)
TinyGPS++ → a built-in NMEA parser; the `SPISlave` callback model → a
hand-written full-duplex pump (full SPI0 reset + 8-byte FIFO preload in the idle
gap, CS-edge realign); no tinyusb. The `SPISlave` transmit path could not stream
reliably as a slave on the bench — that is why this blade is native SDK, and why
it is the reference the radio-blade pico-sdk ports will copy. See `PICO_LESSONS.md`.
