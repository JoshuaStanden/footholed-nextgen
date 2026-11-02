# remix-fh-pico-sdk-sx1262 — SX1262 LoRa blade (native Pico SDK)

> Raspberry Pi **Pico (RP2040)** + Seeed **Wio-SX1262**, native **Raspberry Pi
> Pico C/C++ SDK** (no Arduino layer). A polled SPI **slave** on the P4 hub's
> **Bus B**, speaking the binary **fh_protocol**. **LoRa is the COMMS link to
> the sensor stick — TX is the core job, not a capture radio.** This is the
> pico-sdk port of the arduino sx1262 blade: the hub-link transport is the
> **bench-proven GPS pump copied verbatim** (spi0), and the radio runs on spi1
> through **RadioLib + its official Pico-SDK HAL (PicoHal)**.

**Docs:** `WIRING.md` · `README.md` (this) · `AGENTS.md` (read before editing) ·
`CONTINUITY.md` · `FUTURE.md`. Plus `PICO_LESSONS.md` (verbatim GPS master).

## What it does
- Owns one Wio-SX1262. Transmits payloads (≤200 B) to the sensor stick, opens a
  listen window, and emits inbound LoRa frames to the hub as unsolicited
  indications. One op at a time (BUSY if a second request arrives mid-op).

## How it attaches — two independent SPI blocks
- **Hub link ↔ P4 Bus B:** **spi0 slave, MODE 1** — SCK GP18 / MOSI GP16 /
  MISO GP19 / CS GP21. Byte-identical to the proven GPS pump, with ONE change:
  CS is **polled**, not an edge IRQ (see "The one hard design call" below).
- **Radio ↔ Wio-SX1262:** **spi1 master, MODE 0** — SCK GP10 / MOSI GP11 /
  MISO GP12 + NSS GP13 / RST GP8 / BUSY GP9 / DIO1 GP7 / RF_SW1 GP6.

This is the opposite split from the arduino blade (radio SPI0, hub SPI1): the
SDK port keeps the hub pump on the proven spi0. Full tables in `WIRING.md`.

## Two-core design
- **core0** runs the hub-link SPI-slave pump (GPS blade's full-reset + preload
  + paired RX/TX + CS realign), and is the **single writer** of the outbound
  frame. It answers PING/PING_REQ directly.
- **core1** owns the SX1262 on spi1 via RadioLib + PicoHal. It runs fired TX,
  the listen window (polling for RX → unsolicited RX_IND), and LISTEN on/off,
  producing results into mailboxes core0 stages.

## The one hard design call — bank IRQ / core split
RadioLib's `PicoHal` installs a **shared `IO_IRQ_BANK0` raw handler** for the
SX1262's DIO1 line. The RP2040 won't let that coexist with the GPS pump's
CS-edge callback API. **Resolution:** RadioLib + PicoHal (and its DIO1 IRQ) run
entirely on **core1**; the hub pump on **core0 polls CS** instead of using an
edge IRQ, so the bank IRQ is PicoHal's alone. This is documented in `main.c`
KNOWN RISKS **R-IRQBANK** with a PIO fallback if CS polling can't keep up at
speed. (It is the LoRa analogue of the cc1101 single-writer decision: give the
radio its own core and IRQ bank, keep the hub link self-contained on the other.)

## Build / flash (your hardware only)
CMake + Pico SDK, same pattern as the GPS blade, **plus RadioLib**.

First, get RadioLib at the pinned tag (one-time):
```bash
git clone https://github.com/jgromes/RadioLib.git
cd RadioLib && git checkout 7.8.1 && cd ..
```
Then build, pointing `-DRADIOLIB_PATH` at that checkout:
```bash
cmake -B build -S . -DPICO_BOARD=pico -DCMAKE_BUILD_TYPE=Release \
  -DRADIOLIB_PATH=/path/to/RadioLib -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -G Ninja
cmake --build build
python uf2conv.py build/fh_pico_sx1262.bin -o build/fh_pico_sx1262.uf2
```
`-DRADIOLIB_PATH` points at YOUR RadioLib checkout. **Use RadioLib 7.8.1** (what
the arduino blade pinned). This is a HARD requirement, not a suggestion:
`sx_shim.cpp` calls the positional `SX1262::begin(freq,bw,sf,...)` overload,
which is deprecated-but-present in 7.x and **removed on RadioLib 8.x / master**.
Building against 8.x trips a compile-time `#error` in `sx_shim.cpp` (by design)
— to move to 8.x you must port `begin()` to the `ConfigLoRa_t` form. The build
adds RadioLib as a subdirectory, defines
`RADIOLIB_BUILD_RPI_PICO`, and compiles RadioLib's `src/hal/RPiPico/PicoHal.cpp`.
Three of our TUs: `main.c` (C), `sx_shim.cpp` (C++ RadioLib bridge),
`fh_frame.c`. `PICO_NO_PICOTOOL=1`; `.uf2` via `uf2conv.py`.

**ISM band: 915 MHz (North America), hardcoded** in `sx_init()` — committed, not
a per-region choice.

## What the hub asks / this blade answers
| Hub request | Blade response |
|---|---|
| `FH_TYPE_PING` | `FH_TYPE_PONG` |
| `FH_OD_PING_REQ` (0x20) | `FH_OD_PING_RESP` (0x21) + radios_ready |
| `FH_OD_LORA_TX_REQ` (0x28) | `FH_OD_LORA_TX_RESP` (0x29) status + toa_ms |
| `FH_OD_LORA_LISTEN_REQ` (0x2B) | `FH_OD_LORA_LISTEN_RESP` (0x2C) |
| (while listening) | `FH_OD_LORA_RX_IND` (0x2A) UNSOLICITED |

## Status
Pico-sdk port, reasoned first draft. The hub-link pump is the proven part; the
risk surface is the RadioLib-on-SDK radio (via PicoHal), the bank-IRQ/core
split, and RF bring-up (TCXO, RF_SW1). See `CONTINUITY.md`, `FUTURE.md`, and the
KNOWN RISKS block in `main.c`. The arduino blade `../archive/arduino-blades/remix-fh-pico-sx1262/`
stays until this is bench-proven.

## Licence / credit
RadioLib (jgromes), MIT — incl. its PicoHal. SX1262 bring-up/transmit/receive
sequence carried from the gen1/arduino blade. Retain notices on redistribution.
