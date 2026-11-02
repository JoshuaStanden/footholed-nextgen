# remix-fh-pico-cc1101 — CC1101 sub-GHz blade

> Raspberry Pi **Pico (RP2040)** + **CC1101** OOK sub-GHz radio. A fired SPI
> **slave** on the P4 hub's **Bus B**, speaking the binary **fh_protocol** — the
> same wire contract as the hub, the XIAO sniffers, and the GPS blade. The OOK
> capture/decode/replay core is carried **verbatim** from the proven gen1 blade;
> only the transport changed (ASCII-over-USB → binary SPI). Written by reading
> only — NOT compiled or flashed on the authoring machine.

**This blade's docs:** `WIRING.md` (hookup + bench order) · `README.md` (this —
for people) · `AGENTS.md` (for AI/LLM agents — read before editing) ·
`CONTINUITY.md` (resume point) · `FUTURE.md` (what's next, incl. the pico-sdk
port plan).

## What it does
- Owns one CC1101; captures fixed-code OOK remotes via a GDO0 edge-interrupt +
  `micros()` timing ring, decodes against an RCSwitch-style protocol table, and
  can replay/transmit. Capture/decode is the proven path.
- Answers the hub's on-demand requests, one op at a time (BUSY if a second
  request arrives mid-op).

## How it attaches
Two independent SPI blocks on the one Pico:
- **Radio ↔ CC1101:** hardware SPI0 master (GP18/19/16 + CSN GP17 + GDO0 GP20).
- **Hub link:** hardware SPI1 slave (GP10/12/11/13 + optional IRQ GP8), **MODE 1**.

Full pin tables and the bench bring-up order are in `WIRING.md`. USB `Serial` is
a local debug console only (type `s` for status) — never the hub link.

## Build / flash (your hardware only)
Open `remix-fh-pico-cc1101.ino` in the Arduino IDE with the Earle-Philhower
**arduino-pico** core, board = Raspberry Pi Pico. Library:
**SmartRC-CC1101-Driver-Lib** (ELECHOUSE API) — **pin the version** (SmartRC went
V3.0.0; use an ELECHOUSE-compatible release). The `.ino` name must match the
folder name.

## What the hub asks / this blade answers
| Hub request | Blade response |
|---|---|
| `FH_OD_PING_REQ` (0x20) | `FH_OD_PING_RESP` (0x21) + radios_ready |
| `FH_OD_CC1101_REQ` (0x22) action/freq/dwell | `FH_OD_CC1101_RESP` (0x23) status + decoded data |
| `FH_TYPE_PING` | `FH_TYPE_PONG` |

Actions: LISTEN / RX_ONE / RAW_OOK (capture-to-dump) / REPLAY (re-transmit the
last capture). RAW_OOK streams the edge buffer as `FH_OD_DUMP_IND` chunks, then
the summary RESP last.

## Status
First-pass firmware, reasoned-not-compiled. The radio core is the proven gen1
code; the risk surface is the new SPI-slave hub link. See `CONTINUITY.md` for
what's open and `FUTURE.md` for the planned pico-sdk transport rewrite.

## Licence / credit
OOK timing table + capture→decode adapted from HaleHound-CYD (Jesse C. Hale) /
ESP32-DIV (CiferTech), MIT — retain the notice on redistribution.
