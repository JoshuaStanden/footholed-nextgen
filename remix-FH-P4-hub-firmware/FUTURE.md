# remix-FH-P4-hub-firmware — FUTURE (planning)

Forward-looking notes for the hub. The parked items below are planned work, not
bugs — the hub is framework-first and bench-proven to build/boot/BLE.

## 1. Prove the SPI buses end-to-end
The hub master is built and boots; what is unproven is a frame actually moving to
a real SPI **slave** on each bus. The slave firmwares are separate objects
(XIAO-S3, XIAO-C5, head, and the Pico blades). Bring-up order: one Bus-A peer
first (prove a capture row lands), then the GPS blade on Bus B (prove a 0x02 fix
on the scheduled poll), then the fired radios. Whether SPI3 drains a quad read at
target clock is a bench item.

## 2. The head render-feed over Bus A
The hub's status MODEL exists (`fh_hub_ui.c`); what is parked is the SERIALIZER
that pushes it to `FH_PEER_HEAD` over Bus A as a render feed. The head is a
Bus-A peer tracked for liveness only today (node 4 reads DEAD honestly until head
firmware exists). Writing the head firmware + the hub-side feed is a later pass.

## 3. Bus-A throughput upgrade (link.c R4)
Pass 1 clocks a fixed 261 B per IRQ — wasteful when a peer has one 34 B capture.
The upgrade path is a two-phase length header or a multi-frame drain while IRQ
stays asserted. PARKED until the fixed-size base form is bench-proven.

## 4. Blade transport dependency (cross-reference)
The three arduino-pico RADIO blades (cc1101, nrf24, sx1262) are COMMITTED to a
pico-sdk transport rewrite (decided 2026-11-01) — that is the BLADES' future
(`../remix-PICO-SDK-PORT-PLAN.md` and each blade's `FUTURE.md`), not the hub's.
The hub side (Bus B master, two-phase xfer, requester) is already built and does
NOT change when a blade is ported — the wire contract is frozen.

## 5. Smaller items
- BLE device NAMES to Medusa (they don't fit the 10-byte `extra`; the SD log has
  them). A later Medusa-side change, not a hub blocker.
- SUMMARY (0x03) currently folds into the capture mapping; a distinct SUMMARY row
  shape is parked (same as gen1).
- An external RTC on GPIO7/8 (I2C) would survive cold swaps; the board layer has
  graceful-false shims ready (`fh_board_rtc_*`).
