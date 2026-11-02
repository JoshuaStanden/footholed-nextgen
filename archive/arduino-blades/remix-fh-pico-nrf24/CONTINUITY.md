# remix-fh-pico-nrf24 — CONTINUITY (resume point)

> Resume here if a session drops. Snapshot 2026-11-01. Folder
> `~/Desktop/remix footholed/`. Standing rules in the P4 hub CONTINUITY +
> `../AGENTS.md`: no compile/flash here, remix- prefix, honest deep review,
> `.ino` name == folder. Per-part docs: WIRING / README / AGENTS / this /
> FUTURE.

## What this is
Pico (RP2040) + nRF24L01 blade. A fired SPI SLAVE on the P4 hub's Bus B, binary
fh_protocol. FRAMEWORK ONLY — radio up + link proven, no capture modules.

## Where it stands
- RF24 bring-up (`radioInit`, incl. the `radio.begin(&SPI)` pin-binding H1 fix)
  carried VERBATIM from the proven gen1 framework blade.
- Transport ported ASCII-over-USB → binary fh_frame over a SPI1 slave link,
  reworked to the arduino-pico vendor contract (IRQ copies raw → loop decodes →
  loop stages; `onDataRecv`/`onDataSent`; mutex removed, single-core).
- **SPI MODE corrected 2026-11-01:** MODE 0 → **MODE 1** (RP2040 slave one-byte-
  per-CS rule). Do NOT revert. See `AGENTS.md`.
- A minimal RPD presence scan exercises the REQ/RESP path. SNIFF (mode 1)
  capture-to-dump streams `FH_OD_DUMP_IND`. nRF24 EXT TX (0x3C) implemented;
  JAM/PROTOKILL are extern-author stubs.

## nRF24 TRUTH (carried CORE-3 — do not regress)
No true RSSI — only 1-bit RPD. Scan reports `active_chan` (0xFF=none);
`resp.rssi = 0`. Never invent a dBm.

## Open (bench — cannot resolve by reading)
1. arduino-pico `SPISlave1` on your installed core version (names confirmed vs
   vendor reference; version match is the residual).
2. **SPISlave1 transmit may not stream 261 B even in MODE 1** — the layer that
   sank the GPS blade. This blade is COMMITTED to the pico-sdk port regardless
   (order: after cc1101); a pre-port bench run is a data point, not a gate
   (`FUTURE.md`).
3. Two SPI blocks coexisting (SPI0 master radio + SPI1 slave hub), no pin clash.
4. `testRPD()` (P-variant) vs `testCarrier()` on your RF24 version.
5. 10µF decoupling cap at the module (false-absent fix).

## Not done / later
- Real capture/sniffer modules (head-gated) — deliberately absent; framework
  first.
- The hub-side on-demand requester turning a RESP into a pico-nrf24 CSV row is
  built (hub `fh_hub_req.c`); bench-prove the round trip.
- Pico-sdk transport rewrite — COMMITTED (order: after cc1101); see `FUTURE.md`.
