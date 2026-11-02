# BENCH PLAN — locked 2026-07-31 (Joshua)

## Phase 1 — Minimum wardriving (flash these four, in this order)

1. **ESP32-P4 hub** — flash first; every bench result (CSV rows, BLE/Wi-Fi
   captures, GPS correlation) lands on its SD card. Compile-fix pass expected
   (newest unproven code: logger NVS stage persist, requester SNIFF deadline).
2. **XIAO-S3** — Wi-Fi capture source; proves the offset-36 SSID fix on live
   beacons (garbage SSIDs must NOT appear in the SD recon log / Medusa store).
3. **XIAO-C5** — twin of S3; same SSID assertion, second Wi-Fi band coverage.
4. **Pico + GPS blade** — PROVEN WORKING on the bench 2026-11-01 (native
   pico-sdk, MODE 1, 261-byte stream, CRC-valid GPS_FIX, hub RTC locked,
   PICO=UP). Re-confirm in the wardrive context: fix stream, hub time set on
   first fix, GPS-tagged AP sightings land in Medusa.

PASS = wardrive session shows: GPS-tagged AP sightings with real SSIDs, BLE
advertisers (src-5) in `bt scan`, captures + SSID rows in results.csv.

## Phase 2 — Radio blades (after Phase 1 passes)

The three radio blades (CC1101 / nRF24 / SX1262-LoRa) are arduino-pico sketches,
first-pass. Their hub-link SPI slave is now **MODE 1** (corrected 2026-11-01 —
the RP2040 one-byte-per-CS rule). If a blade shows the byte-0-then-0xff underrun
after MODE 1 is confirmed, the SPISlave1 transmit path is suspect and the fallback
is the native-SDK port the GPS blade proved — see `remix-PICO-SDK-PORT-PLAN.md`.

5. CC1101 — capture (LISTEN/RX_ONE) → replay (REPLAY / replay-from seq).
6. nRF24 — scan, then SNIFF capture-to-dump (proves the chunk stream E2E).

## Rules

- One blade at a time; bring it up fully before adding the next.
- Agent never compiles/flashes — Joshua builds; agent fixes compile errors.
- Fix-then-continue: any compile error or bench failure gets fixed before the
  next item on the list.
