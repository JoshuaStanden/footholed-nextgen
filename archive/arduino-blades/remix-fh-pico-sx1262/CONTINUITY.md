# remix-fh-pico-sx1262 — CONTINUITY (resume point)

> Resume here if a session drops. Snapshot 2026-11-01. Folder
> `~/Desktop/remix footholed/`. Standing rules in the P4 hub CONTINUITY +
> `../AGENTS.md`: no compile/flash here, remix- prefix, honest deep review,
> `.ino` name == folder. Per-part docs: WIRING / README / AGENTS / this /
> FUTURE.

## What this is
Pico (RP2040) + Seeed Wio-SX1262 LoRa blade. A SPI SLAVE on the P4 hub's Bus B,
binary fh_protocol. LoRa is the COMMS link to the sensor stick — TX is CORE,
not a capture sensor.

## The hardware split (don't lose this)
Seeed "XIAO ESP32S3 & Wio-SX1262 Kit" separated: Wio-SX1262 → this Pico blade;
XIAO ESP32-S3 → the 2.4 GHz Wi-Fi sniffer. This Pico drives the SX1262 directly
incl. RF_SW1 (host pin) and TCXO-on-DIO3 (RadioLib begin() sets 1.6 V).

## Where it stands
- RadioLib SX1262 bring-up (explicit begin() + TCXO, CORE-2), transmit(), RF_SW1
  handling, `Module(cs,dio1,rst,busy)` order — carried VERBATIM from gen1.
- Transport ported UART → binary fh_frame over a SPI1 slave link, arduino-pico
  vendor contract (IRQ copies raw → loop decodes/stages; `onDataRecv`/
  `onDataSent`; mutex removed, single-core).
- **SPI MODE corrected 2026-11-01:** MODE 0 → **MODE 1** (RP2040 slave one-byte-
  per-CS rule). Do NOT revert. See `AGENTS.md`.
- Hub-link pins corrected 2026-10-31 to the valid upper spi1 group
  (GP14/24/15/25; the old GP6/4/3/5 was an SPI0 group). Rewire hub side to match.
- Real LISTEN window added (gen1 left it a stub); `service_listen()` gates on
  `checkDataReady()` before `readData()` (non-blocking).

## Open (bench — cannot resolve by reading)
1. arduino-pico `SPISlave1` on your installed core version.
2. **SPISlave1 transmit may not stream 261 B even in MODE 1** — the layer that
   sank the GPS blade. This blade is COMMITTED to the pico-sdk port regardless
   (order: LAST — the RadioLib shim is the hardest); a pre-port bench run is a
   data point, not a gate (`FUTURE.md`).
3. Two SPI blocks coexisting; the upper-spi1-group hub pins need the hub-side
   wiring updated to match.
4. RadioLib begin() TCXO (−707) / wiring (−2); RF_SW1 sense; readData/
   checkDataReady/getPacketLength/getRSSI/getSNR names (RadioLib 7.8.1).
5. The hub must drain unsolicited RX_IND during a listen window.

## Not done / later
- LoRa-as-capture (deferred, not ruled out — not built here).
- The hub-side on-demand requester + RX_IND handling path.
- Pico-sdk transport rewrite — COMMITTED (order: LAST, hardest shim); see
  `FUTURE.md` (the RadioLib-on-SDK shim is the real work).
