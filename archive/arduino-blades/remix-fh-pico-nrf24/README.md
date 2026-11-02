# remix-fh-pico-nrf24 — nRF24 2.4 GHz blade (framework)

> Raspberry Pi **Pico (RP2040)** + **nRF24L01**. A fired SPI **slave** on the P4
> hub's **Bus B**, speaking the binary **fh_protocol**. **FRAMEWORK ONLY** —
> radio up + link proven, no capture modules yet. The RF24 bring-up is carried
> verbatim from the proven gen1 framework blade; only the transport changed
> (ASCII-over-USB → binary SPI). Written by reading only — NOT compiled/flashed
> on the authoring machine.

**This blade's docs:** `WIRING.md` (hookup + bench order) · `README.md` (this —
for people) · `AGENTS.md` (for AI/LLM agents — read before editing) ·
`CONTINUITY.md` (resume point) · `FUTURE.md` (what's next, incl. the pico-sdk
port plan).

## What it does
- Brings up one nRF24L01 and answers the hub's on-demand requests, one op at a
  time (BUSY if a second arrives mid-op).
- A minimal RPD presence **scan** (sweep a channel range, report the first
  channel with a carrier) exercises the REQ/RESP path. A SNIFF mode dwells on
  one channel and captures payloads to a dump stream.
- **No capture/injection/sniffer modules** — those are later, head-gated
  additions. Framework comes first.

## nRF24 reality (do not regress)
The nRF24 has **no true RSSI** — only a 1-bit carrier-detect (RPD). The scan
reports `active_chan` (0xFF = none); the `rssi` field stays 0. Never invent a
dBm the chip cannot measure.

## How it attaches
Two independent SPI blocks on the one Pico:
- **Radio ↔ nRF24:** hardware SPI0 master (GP18/19/16 + CSN GP17 + CE GP20).
- **Hub link:** hardware SPI1 slave (GP10/12/11/13 + optional IRQ GP8), **MODE 1**.

Full pin tables and the bench order are in `WIRING.md`. A **10µF cap** across
the nRF24 VCC/GND is strongly recommended. USB `Serial` is a local debug console
only (`s` for status) — never the hub link.

## Build / flash (your hardware only)
Open `remix-fh-pico-nrf24.ino` in the Arduino IDE with the Earle-Philhower
**arduino-pico** core, board = Raspberry Pi Pico. Library: **RF24 by TMRh20**.
The `.ino` name must match the folder name.

## What the hub asks / this blade answers
| Hub request | Blade response |
|---|---|
| `FH_OD_PING_REQ` (0x20) | `FH_OD_PING_RESP` (0x21) + radios_ready |
| `FH_OD_NRF24_REQ` (0x24) mode/chan/dwell | `FH_OD_NRF24_RESP` (0x25) active_chan (SCAN) or dump stream (SNIFF) |
| `FH_TYPE_PING` | `FH_TYPE_PONG` |

nRF24 EXT TX (0x3C) is implemented (transmit to your own receiver);
JAM/PROTOKILL are extern-author stubs (`FH_OD_EXT_STUB`).

## Status
First-pass framework, reasoned-not-compiled. The RF24 bring-up is the proven
gen1 code; the risk surface is the new SPI-slave hub link. See `CONTINUITY.md`
for what's open and `FUTURE.md` for the planned pico-sdk transport rewrite.
