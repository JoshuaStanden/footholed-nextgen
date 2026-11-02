# remix-fh-pico-cc1101 — CONTINUITY (resume point)

> Resume here if a session drops. Snapshot 2026-11-01. Folder
> `~/Desktop/remix footholed/`. Standing rules in the P4 hub CONTINUITY +
> `../AGENTS.md`: no compile/flash here, remix- prefix, honest deep review,
> `.ino` name == folder. Per-part docs: WIRING / README / AGENTS / this /
> FUTURE.

## What this is
Pico (RP2040) + CC1101 OOK sub-GHz blade. A fired SPI SLAVE on the P4 hub's
Bus B, binary fh_protocol. Holds the project's real OOK capture/decode logic
(the deepest firmware — do not lose it).

## Where it stands
- Radio core (RCSwitch-style timing table, GDO0 edge-IRQ capture ring,
  `decodeSignal`, `transmitCode`, `radioEnterRx/Tx`) carried VERBATIM from the
  proven gen1 blade.
- Transport ported ASCII-over-USB → binary fh_frame over a SPI1 slave link,
  reworked to the arduino-pico vendor contract (IRQ copies raw → loop decodes →
  loop stages; `onDataRecv`/`onDataSent`; mutex removed, single-core).
- **SPI MODE corrected 2026-11-01:** MODE 0 → **MODE 1** (RP2040 slave one-byte-
  per-CS rule). Do NOT revert. See `AGENTS.md`.
- REPLAY (action 3) and RAW_OOK capture-to-dump (action 2, `FH_OD_DUMP_IND`
  chunk stream) are wired. CC1101 EXT TX (0x34) implemented; JAM/BRUTE are
  extern-author stubs (`FH_OD_EXT_STUB`).

## Open (bench — cannot resolve by reading)
1. arduino-pico `SPISlave1` on your installed core version (names confirmed vs
   vendor `SPItoMyself` reference; version match is the residual).
2. **SPISlave1 transmit may not stream 261 B even in MODE 1** — the layer that
   sank the GPS blade. This blade is COMMITTED to the pico-sdk port regardless
   (cc1101 is the pilot); a pre-port bench run is a data point, not a gate
   (`FUTURE.md`).
3. Two SPI blocks coexisting (SPI0 master radio + SPI1 slave hub), no pin clash.
4. Pin an ELECHOUSE-compatible SmartRC-CC1101 version.
5. `getRssi()` method name on your driver.

## Not done / later
- The hub-side on-demand requester turning a RESP into a pico-cc1101 CSV row is
  built (hub `fh_hub_req.c`); bench-prove the round trip.
- Pico-sdk transport rewrite — COMMITTED (cc1101 is the pilot); see `FUTURE.md`
  and `../remix-PICO-SDK-PORT-PLAN.md`.
