# remix-fh-pico-sdk-cc1101 — CC1101 sub-GHz blade (native Pico SDK)

> Raspberry Pi **Pico (RP2040)** + **CC1101** OOK sub-GHz radio, native
> **Raspberry Pi Pico C/C++ SDK** (no Arduino layer). A polled SPI **slave** on
> the P4 hub's **Bus B**, speaking the binary **fh_protocol** — the same wire
> contract as the hub, the XIAO sniffers, and the GPS blade. This is the
> **pilot pico-sdk port** of the arduino-pico cc1101 blade: the hub-link
> transport is the **bench-proven GPS pump copied verbatim**, and the radio runs
> on a second SPI block via a **vetted SDK CC1101 driver**. Written by reading
> only — **NOT compiled or flashed** on the authoring machine.

**This blade's docs:** `WIRING.md` (hookup + bench order) · `README.md` (this —
for people) · `AGENTS.md` (for AI/LLM agents — read before editing) ·
`CONTINUITY.md` (resume point) · `FUTURE.md` (what's next). Plus
`PICO_LESSONS.md` — the hard-won RP2040 lessons (copied verbatim from the GPS
master); read it before debugging any Pico blade link problem.

## What it does
- Owns one CC1101; captures fixed-code OOK remotes via a GDO0 edge-interrupt +
  `time_us_32()` timing ring, decodes against an RCSwitch-style protocol table,
  and can replay/transmit. The capture/decode/transmit core is carried from the
  proven gen1/arduino blade, adapted to SDK timing.
- Answers the hub's on-demand requests, one op at a time (BUSY if a second
  request arrives mid-op).

## How it attaches — two independent SPI blocks
- **Hub link ↔ P4 Bus B:** **spi0 slave, MODE 1** — SCK GP18 / MOSI GP16 /
  MISO GP19 / CS GP21. Byte-identical to the bench-proven GPS blade pump.
- **Radio ↔ CC1101:** **spi1 master, MODE 0** — SCK GP10 / MOSI GP11 /
  MISO GP12 / CSN GP13 + GDO0 GP14.

Note this is the **opposite split** from the arduino blade (which had radio on
SPI0, hub on SPI1): the SDK port keeps the hub pump on the proven spi0 and moves
the radio to spi1. Full pin tables and bench bring-up order are in `WIRING.md`.

## Two-core design
- **core0** runs the hub-link SPI-slave pump (the GPS blade's proven full-duplex
  261-byte transaction: SPI0 reset + 8-byte FIFO preload + paired RX/TX push +
  CS-edge realign). It is the **single writer** of the outbound frame, and it
  answers PING/PING_REQ directly.
- **core1** owns the CC1101 on spi1 and runs the fired op (a capture can take
  seconds, so it lives off the pump). It produces results into a mailbox; core0
  stages them. This mirrors the GPS blade's core split.

## Build / flash (your hardware only)
CMake + Pico SDK, same pattern as the GPS blade (see that README for the exact
Windows/git-bash invocation). The build links three translation units:
`main.c` (C), `ELECHOUSE_CC1101_SRC_DRV.cpp` (the vetted C++ driver), and
`cc_shim.cpp` (the extern-"C" bridge + OOK transmit). `PICO_NO_PICOTOOL=1` is
set and the `.uf2` is made from the `.bin` with the bundled `uf2conv.py`.

The driver is **SmartRC-CC1101-Driver-Lib-Pico** (sentoxo's SDK fork of
LSatan/ELECHOUSE, MIT), vendored and **pinned** here with two clearly-marked
FH-EDITs (SPI bus → spi1, `getCC1101()` pin-clobber guard). See `AGENTS.md`.

## What the hub asks / this blade answers
| Hub request | Blade response |
|---|---|
| `FH_TYPE_PING` | `FH_TYPE_PONG` |
| `FH_OD_PING_REQ` (0x20) | `FH_OD_PING_RESP` (0x21) + radios_ready |
| `FH_OD_CC1101_REQ` (0x22) action/freq/dwell | `FH_OD_CC1101_RESP` (0x23) status + decoded data |
| `FH_OD_EXT_CC_TX_REQ` (0x34) | `FH_OD_EXT_CC_TX_RESP` (0x35) echo (IMPLEMENTED) |
| `FH_OD_EXT_CC_JAM/BRUTE` (0x30/0x32) | RESP `FH_OD_EXT_STUB` (not authored) |

Actions: LISTEN / RX_ONE / RAW_OOK (capture-to-dump) / REPLAY (re-transmit the
last capture). RAW_OOK streams the edge buffer as `FH_OD_DUMP_IND` chunks (one
per hub transaction), then the summary RESP last.

## Status
Pilot pico-sdk port, **reasoned-not-compiled, NOT bench-tested.** The hub-link
pump is the proven part (copied from the working GPS blade); the risk surface is
the radio half — the spi1 bring-up, the GDO0 capture IRQ, decode, and the vetted
(but 0-star) driver on real silicon. See `CONTINUITY.md` for the resume point,
`FUTURE.md` for what's next, and the KNOWN RISKS block at the end of `main.c`.
The arduino blade `../archive/arduino-blades/remix-fh-pico-cc1101/` stays until this is bench-proven.

## Licence / credit
OOK timing table + capture→decode adapted from HaleHound-CYD (Jesse C. Hale) /
ESP32-DIV (CiferTech), MIT. CC1101 driver: SmartRC-CC1101-Driver-Lib-Pico
(sentoxo / LSatan / ELECHOUSE), MIT. Retain the notices on redistribution.
