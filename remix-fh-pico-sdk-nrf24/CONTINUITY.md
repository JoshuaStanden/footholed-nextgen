# CONTINUITY.md — remix-fh-pico-sdk-nrf24 (resume point)

Snapshot for picking this blade back up. Pair with `AGENTS.md`, `WIRING.md`,
`FUTURE.md`, and the KNOWN RISKS block in `main.c`.

## Where this blade stands (2026-11-01)
- **Pico-sdk port of the arduino nrf24 framework blade — first draft, complete.**
  This is the LAST of the three committed radio ports (cc1101 → sx1262 → nrf24;
  sx1262 was pulled ahead of nrf24 earlier for the sensor-stick dependency).
- **Hub transport = the bench-proven GPS pump on spi0**, standard CS-edge-ISR
  form (no RadioLib, so no CS-polling workaround).
- **Radio = a hand-written FH-owned nRF24 driver (`nrf_shim.c`) on spi1.**

## Why a hand-written driver (the key decision)
No clean permissively-licensed pico-sdk nRF24 driver exposes what the verbs need
(RPD read for SCAN, raw payload capture RX for SNIFF):
- `AndyRids/pico-nrf24` — native pico-sdk, clean API, but **GPL-2.0** → rejected
  (would force the whole project to GPL; every other driver here is MIT).
- TMRh20 `RF24` — non-Arduino path is Linux spidev, not pico-sdk.
So `nrf_shim.c` was written against the Nordic nRF24L01+ datasheet register map
— FH-owned, permissive, minimal (only the ops the verbs use). User accepted the
licensing direction (2026-11-01).

## Files in this package
- `main.c` — core0 hub pump (CS-edge ISR) + core1 nRF24 executor; SCAN, SNIFF
  (capture-to-dump), ext-band TX; single-writer staging.
- `nrf_shim.c` / `nrf_shim.h` — the hand-written driver: present/init/
  set_channel/rx_mode/idle/rpd/rx_payload/tx_payload.
- `fh_pico_pins_sdk.h` — hub spi0 (GP18/16/19/21), radio spi1 (GP10/11/12 +
  CSN13/CE7).
- `CMakeLists.txt` — all C, no external deps, no C/C++ bridge.
- `fh_frame.c/.h`, `fh_ondemand.h`, `newlib_lock_stubs.c`, `uf2conv.py` —
  byte-identical GPS copies.
- `PICO_LESSONS.md` — verbatim GPS master (md5 `a39f5c37212e6e9c584f5e5dcf2e5511`).

## Verbs wired
`FH_TYPE_PING→PONG`, `FH_OD_PING_REQ→RESP`, `FH_OD_NRF24_REQ→RESP`
(SCAN mode 0 / SNIFF mode 1), `FH_OD_DUMP_IND` chunk stream for SNIFF (40-byte
records), ext-band NRF TX `0x3C→0x3D` (IMPLEMENTED), JAM/PROTOKILL
`0x38/0x3A` → `FH_OD_EXT_STUB`.

## Design decisions locked in (do not revert)
- SPI split inverted vs the arduino blade: hub spi0 (proven), radio spi1.
- core0 single writer; core1 produces into mailboxes.
- core0 never touches spi1; PING_REQ reads cached `s_radio_ready`.
- `nrf_shim.c` stays FH-owned/permissive — NO GPL nRF24 code.
- No real RSSI — SCAN reports active_chan only.

## What is NOT done (bench + follow-ups)
- Nothing bench-proven. The hand-written driver (R-DRIVER), the promiscuous
  sniff approximation (R-PROMISC), and the SCAN RPD timing are all unverified —
  see `main.c` KNOWN RISKS. This blade warrants a closer register-by-register
  review than the vendored-driver blades, because the driver is from scratch.
- The arduino blade `../archive/arduino-blades/remix-fh-pico-nrf24/` stays until bench-proven.

## Next session should
1. With bench results: work the `WIRING.md` failure tree; the driver (R-DRIVER)
   is the first suspect for any radio-side failure.
2. All three radio ports now exist as first drafts. The remaining program-level
   item is the user's pending decision to retire the arduino blades once the SDK
   ports are bench-proven.

## Multi-check round (2026-11-01, whole-repo sweep) — 1 fix, 2 claims rejected
- **FIXED (functional):** `nrf_tx_payload` inherited the current CONFIG's CRC
  bit. A SNIFF writes CRC OFF, so an ext-band TX after a sniff went out CRC-less
  and is silently dropped by standard nRF24 receivers. TX now writes an explicit
  `PWR_UP | EN_CRC | CRCO` (PRIM_RX=0). No side effects: every RX entry rewrites
  CONFIG via `nrf_rx_mode()`.
- **FIXED (doc):** `nrf_shim.h` claimed TX "leaves the radio back in the prior
  RX/idle state" — false; it leaves TX-standby. Comment corrected (callers
  re-enter RX via `nrf_rx_mode()`, which main.c already does).
- **REJECTED — add ACTIVATE(0x50,0x73) to unlock FEATURE:** FEATURE's reset value
  is 0x00, which is exactly what capture wants; on a locked non-plus part the
  write simply no-ops and FEATURE stays 0. EN_AA is NOT a locked register. Sending
  ACTIVATE is a toggle on non-plus parts (a second send re-locks), so adding it
  adds risk for no gain.
- **REJECTED — CONFIG read-modify-write "overwrites CRC":** the OR preserves the
  CRC bit just written; SPI register reads are immediate (no settle window).
