# remix-fh-pico-sdk-nrf24 — nRF24 2.4 GHz blade (native Pico SDK)

> Raspberry Pi **Pico (RP2040)** + **nRF24L01(+)**, native **Raspberry Pi Pico
> C/C++ SDK** (no Arduino layer). A polled SPI **slave** on the P4 hub's **Bus
> B**, speaking the binary **fh_protocol**. This is the pico-sdk port of the
> arduino nrf24 framework blade: the hub-link transport is the **bench-proven
> GPS pump copied verbatim** (spi0), and the radio runs on spi1 via a
> **hand-written, FH-owned nRF24 driver** (`nrf_shim.c`).

**Docs:** `WIRING.md` · `README.md` (this) · `AGENTS.md` (read before editing) ·
`CONTINUITY.md` · `FUTURE.md`. Plus `PICO_LESSONS.md` (verbatim GPS master).

## What it does
- Owns one nRF24L01(+). **SCAN:** sweep a channel range and report the first
  channel with a carrier (RPD). **SNIFF:** dwell on one channel and
  capture-to-dump every payload seen. **ext-band TX (0x3C):** transmit a
  payload to your own receiver. One op at a time (BUSY otherwise).
- **No real RSSI.** The nRF24 only has a 1-bit RPD/carrier-detect, so SCAN
  reports the active CHANNEL, never a dBm. (The RESP's `rssi` field stays 0 by
  design.)

## Why a hand-written driver (not a vendored library)
cc1101 got a clean MIT SDK driver and sx1262 got RadioLib's official PicoHal.
For the nRF24 there is **no clean permissively-licensed pico-sdk driver** that
exposes what we need (RPD read for SCAN, raw payload capture RX for
SNIFF): `AndyRids/pico-nrf24` is **GPL-2.0** (rejected — it would force the whole
project to GPL), and TMRh20 `RF24`'s non-Arduino path is Linux spidev, not
pico-sdk. So `nrf_shim.c` is a small **FH-owned, permissive** driver written
against the Nordic nRF24L01+ datasheet register map — only the handful of ops
the verbs use. (This also keeps the whole blade one language — C — with no
C/C++ bridge, the simplest of the three radio blades to build.)

## How it attaches — two independent SPI blocks
- **Hub link ↔ P4 Bus B:** **spi0 slave, MODE 1** — SCK GP18 / MOSI GP16 /
  MISO GP19 / CS GP21. Byte-identical to the proven GPS pump, CS-edge-ISR form
  (no RadioLib here, so no bank-IRQ workaround — contrast sx1262).
- **Radio ↔ nRF24:** **spi1 master, MODE 0** — SCK GP10 / MOSI GP11 /
  MISO GP12 + CSN GP13 / CE GP7. (Opposite split from the arduino blade.)

## Two-core design
- **core0** runs the hub-link SPI-slave pump (the GPS blade's full-duplex
  261-byte transaction) and is the **single writer** of the outbound frame.
- **core1** owns the nRF24 on spi1 via `nrf_shim.c`; runs the fired SCAN/SNIFF
  op and ext-band TX, producing results (and, for SNIFF, a dump buffer) into
  mailboxes core0 stages.

## Build / flash (your hardware only)
CMake + Pico SDK, same pattern as the GPS/cc1101 blades — **no extra
dependency** (the nRF24 driver is in-tree). The build links `main.c`,
`nrf_shim.c`, `fh_frame.c`, `newlib_lock_stubs.c`. `PICO_NO_PICOTOOL=1`; `.uf2`
via `uf2conv.py`. See `../remix-BUILD-TOOLCHAIN.md` for the full recipe.

## What the hub asks / this blade answers
| Hub request | Blade response |
|---|---|
| `FH_TYPE_PING` | `FH_TYPE_PONG` |
| `FH_OD_PING_REQ` (0x20) | `FH_OD_PING_RESP` (0x21) + radios_ready |
| `FH_OD_NRF24_REQ` (0x24) SCAN/SNIFF | `FH_OD_NRF24_RESP` (0x25) + active_chan |
| (SNIFF) | `FH_OD_DUMP_IND` (0x2F) chunk stream, then the RESP |
| `FH_OD_EXT_NRF_TX_REQ` (0x3C) | `FH_OD_EXT_NRF_TX_RESP` (0x3D) echo (IMPLEMENTED) |
| `FH_OD_EXT_NRF_JAM/PROTOKILL` (0x38/0x3A) | RESP `FH_OD_EXT_STUB` (not authored) |

## Status
Pico-sdk port, reasoned first draft. The hub-link pump is the proven part; the
risk surface is the **hand-written nRF24 driver** (unproven on silicon) and the
**capture sniff** (see R-PROMISC). See `CONTINUITY.md`, `FUTURE.md`, and the
KNOWN RISKS block in `main.c`. The arduino blade `../archive/arduino-blades/remix-fh-pico-nrf24/` stays
until this is bench-proven.

## Known limitations (plain English — for a human or AI at the bench)
The terse `R-*` tags in `main.c`'s KNOWN RISKS block mean this. They are NOT
to-do items unless noted — most are honest limits so nothing surprises you.

- **R-DRIVER — "the radio driver is hand-written and never run on hardware."**
  `nrf_shim.c` was reasoned from the Nordic datasheet, not taken from a tested
  library. THIS IS THE ONE THAT MATTERS. **Action: bench-test first.** If the
  radio doesn't respond (PING shows the nRF24 bit clear, or SCAN/SNIFF find
  nothing with a known transmitter running), suspect this FIRST — a wrong
  register bit or a CE/power-up timing issue. Everything else on this blade (the
  hub link) is the proven GPS pump.

- **R-FIFO — "fast traffic may drop a few packets while sniffing."** The nRF24
  holds only 3 packets and the code polls every 1 ms; a fast burst can outrun
  it. **Action: usually nothing** — fine for "what's on this channel". Only
  matters if you need EVERY packet; the fix is wiring the nRF24 IRQ pin (see
  FUTURE.md).

- **R-PROMISC — "the sniffer is not a true wiretap, and every capture reads as
  32 bytes."** It only hears packets sent to ONE address (the default
  0xE7E7E7E7E7 — which is what this blade's own TX uses, so bench self-test
  works), not all nearby 2.4 GHz traffic. And because of how the nRF24 works in
  capture mode, there's no per-packet length — each record is a fixed 32 bytes
  (trailing bytes past the real payload are filler). **Action: nothing**, unless
  you expected all-traffic capture or exact lengths — both need a more advanced
  2-byte-address hack we can add later (FUTURE.md).

- **R-RSSI — "the chip can't measure signal strength, only yes/no."** The nRF24
  has no dBm meter, just a 1-bit "is something there" flag. **Action: nothing** —
  SCAN tells you WHICH channel is busy, never how strong. Chip limitation, not
  our code.

**So what do you actually do?** 1) Build it (same recipe as the other blades,
see `../remix-BUILD-TOOLCHAIN.md`). 2) On the bench, confirm the radio responds
(boot blinks + PING). 3) With a known nRF24 transmitter, confirm SCAN finds its
channel and SNIFF captures payloads. 4) If the radio side misbehaves, R-DRIVER
is suspect #1.

## Licence / credit
`nrf_shim.c` is FH-owned (permissive), written against the public Nordic
nRF24L01+ datasheet — no third-party driver code is included. SCAN/SNIFF/TX verb
logic carried from the gen1/arduino framework blade.
