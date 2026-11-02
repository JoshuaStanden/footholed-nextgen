# FUTURE.md — remix-fh-pico-sdk-sx1262 (what's next)

Forward notes for the LoRa blade. Pair with `CONTINUITY.md` and
`../remix-PICO-SDK-PORT-PLAN.md`.

## Place in the plan
Committed order was cc1101 → nrf24 → sx1262 (sx1262 last, hardest shim). **It
was pulled ahead of nrf24** because LoRa is the critical-path comms link to the
sensor stick (still being built). The RadioLib-on-SDK shim turned out tractable
(RadioLib's own PicoHal), so the "hardest, do last" reasoning weakened. nrf24
remains to port.

## Bench-gated (must prove on hardware)
1. **First build** with `-DRADIOLIB_PATH` → confirm RadioLib compiles against
   the SDK, the mixed C/C++ link resolves (R-LINK/R-RLIB), and `PicoHal.cpp`
   builds.
2. **Hub link carries** over the copied (CS-polled) spi0 pump: PING→PONG,
   PING_REQ→RESP. **R-IRQBANK:** watch that CS polling keeps up at 8 MHz/261 B.
3. **Radio bring-up:** `begin()` returns OK (−707 = TCXO, −2 = SPI/DIO wiring);
   PING_RESP shows the SX1262 bit.
4. **LoRa TX** to the sensor stick (≤200 B) → TX_RESP + toa_ms; verify RF_SW1
   sense (R-RFSW) and antenna.
5. **Listen window** → unsolicited RX_IND from the stick. **R-RXPOLL:** confirm
   `getPacketLength()` latches a received frame; if not, add the explicit
   IRQ-flag gate in `sx_rx_poll` (the arduino blade used `checkDataReady()`).

## If R-IRQBANK polling slips
Fallback (NOT built): move the hub pump's CS-boundary realign to a core0 PIO
program (frees the bank IRQ for PicoHal without an SDK GPIO IRQ on core0).
Prefer this over trying to share `IO_IRQ_BANK0` between PicoHal and a CS edge
handler — that sharing is what R-IRQBANK avoids.

## Smaller items
- **ISM band:** intentionally hardcoded **915 MHz (North America)** in
  `sx_init()`. Do NOT add a region build-flag or 868 path — the rig operates in
  North America and 915 is committed. (If that ever changes, it's a one-line
  edit, not a config surface worth building.)
- **LoRa params:** SF9/BW125/CR7 carried from the arduino blade; tune for
  range vs airtime once the link works.
- **Release artifact:** add a `releases/` prebuilt `.uf2` once bench-proven
  (note it depends on the pinned RadioLib tag).
- Bundling RadioLib: currently user-pointed via `-DRADIOLIB_PATH`. If a
  self-contained zip is wanted later, vendor a pinned RadioLib snapshot under
  the package (large) — weigh against keeping it external.

## Explicitly rejected (do not resurrect)
- Redesigning the hub pump (it is the proven GPS pump; only CS→polled changed).
- Staging from core1 (core0 is the single writer).
- A CS edge IRQ on core0 (collides with PicoHal's bank IRQ — R-IRQBANK).
- Calling tone()/noTone() (R-TONE: resets core1).
- Unifying the two SPI modes (hub MODE 1 / radio MODE 0 both correct).
