# remix-fh-pico-sx1262 — SX1262 LoRa blade (comms link)

> Raspberry Pi **Pico (RP2040)** + Seeed **Wio-SX1262**. A SPI **slave** on the
> P4 hub's **Bus B**, speaking the binary **fh_protocol**. LoRa is the **comms
> link to the sensor stick** — TX is CORE, not a capture sensor. Ported from the
> SX1262 half of the gen1 rescue blade; the RadioLib bring-up/transmit is carried
> verbatim, only the transport changed (UART → binary SPI). Written by reading
> only — NOT compiled/flashed on the authoring machine.

**This blade's docs:** `WIRING.md` (hookup + bench order) · `README.md` (this —
for people) · `AGENTS.md` (for AI/LLM agents — read before editing) ·
`CONTINUITY.md` (resume point) · `FUTURE.md` (what's next, incl. the pico-sdk
port plan).

## The hardware split (context)
The Seeed "XIAO ESP32S3 & Wio-SX1262 Kit" was separated: the **Wio-SX1262**
pairs with this Pico (this blade); the **XIAO ESP32-S3** became the 2.4 GHz
Wi-Fi sniffer (`../remix-FH-XIAO-S3-firmware`). This Pico drives the SX1262
directly, incl. RF_SW1 (host pin) and the TCXO-on-DIO3 supply.

## What it does
- Brings up one Wio-SX1262 (RadioLib, explicit `begin()` with TCXO 1.6 V) and
  answers the hub's LoRa verbs, one op at a time (BUSY mid-op).
- `FH_OD_LORA_TX_REQ` — transmit ≤200 B to the stick → `TX_RESP` with `toa_ms`.
- `FH_OD_LORA_LISTEN_REQ` — open/close an RX window (window_ms, 0 = continuous).
- While listening, inbound frames arrive as unsolicited `FH_OD_LORA_RX_IND`
  (rssi/snr/data).

## How it attaches
Two independent SPI blocks on the one Pico:
- **Radio ↔ SX1262:** hardware SPI0 master (GP18/19/16 + NSS GP17 + RST GP12 +
  BUSY GP11 + DIO1 GP10 + RF_SW1 GP9).
- **Hub link:** hardware SPI1 slave, upper group (GP14/24/15/25 + optional IRQ
  GP2), **MODE 1**.

Full pin tables and the bench order are in `WIRING.md`. **Keep the antenna
attached before any TX** (+22 dBm PA). USB `Serial` is a local debug console
only — never the hub link.

## Build / flash (your hardware only)
Open `remix-fh-pico-sx1262.ino` in the Arduino IDE with the Earle-Philhower
**arduino-pico** core, board = Raspberry Pi Pico. Library: **RadioLib 7.8.1**
(pin the version). The `.ino` name must match the folder name.

## Status
First-pass firmware, reasoned-not-compiled. The RadioLib bring-up/transmit is the
proven gen1 code; the risk surface is the new SPI-slave hub link. See
`CONTINUITY.md` for what's open and `FUTURE.md` for the planned pico-sdk
transport rewrite (note: the RadioLib-on-SDK shim is the real work there).

## Authorization
LoRa TX is authorized-use-only, gated on the hub/head, never auto. Antenna
mandatory before TX.
