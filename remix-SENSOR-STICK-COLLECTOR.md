# remix-SENSOR-STICK-COLLECTOR.md — the NextGen deck as LoRa/BLE collector

**Status: THEORY / interface record. No collector code written.** This records a
FROZEN EXTERNAL CONTRACT the NextGen rig consumes; it does not design anything new.

## What the sensor stick is (external sub-project)
`FootHoled-Sensor-Stick` — a separate, battery-powered, 3D-printed RF probe that
passively sniffs Wi-Fi + BLE, logs to microSD (onboard-flash fallback), and sits
idle waiting for a COLLECTOR to come pull its log. It is the SENSOR; the COLLECTOR
is a different device.
- **`main` branch (2026-11-02, THE ONLY BUILD — Heltec retired):** XIAO ESP32-C5
  (dual-band 2.4+5 GHz) + stacked Wio-SX1262 LoRa hat as the workhorse, + XIAO
  ESP32-C3 BLE. First bringup draft.
  https://github.com/JoshuaStanden/FootHoled-Sensor-Stick
- **`heltec-legacy` branch:** the original Heltec WiFi LoRa 32 V4 workhorse +
  XIAO-C3 BLE scanner (2.4 GHz only), kept for history/fallback — no longer the
  active build.
- The stick is effectively a miniature self-contained FootHoled (same C5/C3/SX1262
  parts family as the NextGen peers).

## THE KEY FACT: the handoff contract is FROZEN and build-agnostic
The handoff contract, LoRa parameters and BLE UUIDs are FIXED across builds
(the pre-XIAO Heltec build shared them; XIAO keeps them unchanged). So:
- The NextGen collector implements **ONE** protocol and talks to EVERY stick
  variant (Heltec-legacy or XIAO, present or future). The transport contract is the
  stable interface; stick internals are free to evolve behind it.
- This is NOT a protocol the NextGen project has to design. It is already
  specified (twice). The NextGen work is implementing the COLLECTOR END only.

## The frozen handoff contract (what the deck-as-collector implements)
Session (same over both transports):
```
1. collector → sensor : "GET"                             (begin collection)
2. sensor → collector : "HELLO,<id>,<totalBytes>,<crc32hex>"
3. sensor → collector : log bytes in chunks
4. sensor → collector : "END"
5. collector → sensor : "DONE,<crc32hex>"   (only if collector's CRC matches)
6. sensor deletes its log IFF the DONE CRC matches.
```
- **Safety:** whole log covered by **CRC32** (standard convention: seed
  `0xFFFFFFFF`, reflected, final XOR — any off-the-shelf CRC32 matches). The
  sensor deletes ONLY on a confirmed-matching `DONE`. A drop / mismatch / timeout
  leaves the log intact for the next attempt. Non-destructive by design.
- **LoRa framing:** each chunk `D,<seq>,<rawbytes>` (≤180 B), acked per-chunk
  `A,<seq>`; sender resends on timeout; `seq` = gap detection + in-order
  reassembly.
- **BLE framing:** bonded + encrypted connection (payload is MAC addresses — pair
  on first contact); log bytes stream raw over a NOTIFY characteristic (the
  connection layer handles ordering/chunking).
- **BLE UUIDs (must match EXACTLY):**
  - service `6b1a0001-9d4e-4c2a-8f33-7c2e5a9b1d10`
  - data/NOTIFY `6b1a0002-…`  ·  ctrl/WRITE `6b1a0003-…`
- **LoRa params must match EXACTLY on both ends:** `LORA_FREQ` = **915.0** (US/NA
  — consistent with the NextGen fixed-915 rule), plus `LORA_SF / LORA_BW /
  LORA_CR / LORA_SYNC` (stick defaults). The collector config must carry these.
- **Log format pulled:** lines `<secs>,WIFI,<mac>,<rssi>,ch<n>` and
  `<secs>,BT,<mac>,<addrType>,<rssi>,<name>,<svcUUIDs>,<svcData>,<mfgData>`;
  timestamps are seconds-since-boot — the collector (the deck HAS a clock via GPS)
  can re-stamp with real time on ingest.

## The deck is the natural collector
- The NextGen rig already has the **two transports the contract uses**: the
  **SX1262 blade** (LoRa, far/slow) and the P4's onboard **C6 BLE** (near/fast).
  No new radio is needed to be the collector.
- **Collector CHOOSES the transport** per the contract (LoRa to pull from a
  distance; BLE when you walk up to the stick). The deck decides.
- Design question still open (owner's call): is the deck itself THE collector, or
  is there also a separate small collector (the stick README floats "a Waveshare
  ESP32-C3 + LoRa")? The contract is identical either way; this only affects what
  the head's LoRa pill is driving. Working assumption: the deck is the collector.

## NODE_ID = the stick-registry key (head "manage sticks" overlay)
- Each stick has a unique **`NODE_ID`** string (set before flashing). It appears
  in `HELLO,<id>,…` and in the log.
- The head's LoRa-pill overlay registry is keyed by `NODE_ID`: one row per known
  stick — last-seen, pending log size (from `HELLO`'s `<totalBytes>`), link
  state. Build-agnostic: a Heltec node and a XIAO node are just two `NODE_ID`s.

## COLLECTOR-INITIATES constraint (shapes the overlay's verbs)
- The stick **idles listening on BOTH radios at once** (LoRa RX + BLE advertising)
  and only answers what the collector INITIATES. It never pushes; it pauses Wi-Fi
  sniffing only during a transfer.
- Therefore the head's "manage sticks" action is **"go collect from node X"**
  (deck sends `GET`), NOT "wait for a stick to call in." The overlay is an
  outbound-pickup console: pick a stick (or scan for sticks in range), choose/auto
  LoRa-vs-BLE, fire the collection, watch the CRC-verified transfer, confirm
  delete. The pill's state mirrors the contract's link states
  (`listen`/`BLE~`/`LoRa`/`BLE`).

## Mission profile (what a deployed stick is FOR)
A stick is a stationary, battery-powered, drop-and-leave PASSIVE node that builds
a presence record of a fixed location over days: it fingerprints the devices and
vehicles that appear nearby and logs them for later collection by the deck. The
analysis value is **returning-presence detection** — the same fingerprint seen
across multiple visits indicates a recurring device/vehicle at that spot. Purely
passive, owner-perimeter use; the stick's own repo carries the legal/ethical note
(passive capture, only where you have the right, don't profile identifiable people
beyond need).

### Optional CC1101 sub-GHz add (TPMS) — the DURABLE identity anchor
Theory, not yet on the stick. Adding a CC1101 lets the stick capture **TPMS**
tyre-sensor transmissions, which is valuable here for one specific reason:
- **TPMS sensor IDs do NOT randomize** (fixed for the sensor's multi-year life),
  whereas modern phones **randomize their Wi-Fi/BLE MACs** (the stick README flags
  this). So across repeat visits the PHONE fingerprint degrades but the VEHICLE
  fingerprint persists — TPMS is the **most durable re-identification key** in the
  kit, the anchor the Wi-Fi/BLE observations correlate around.
- **Camp on 315 MHz (NA TPMS), do NOT hop.** TPMS is the only sub-GHz use
  foreseen for the stick, so the CC1101 just parks on 315 and listens. (NextGen's
  own CC1101 blade defaults to 433.92 for OOK remotes — different band, different
  job; the stick's TPMS CC1101 would camp 315.)
- **Cadence, not decode, is the limit.** A packet is ~10-20 ms of airtime and
  decodes instantly; the wait is the sensor's transmit interval (~30-60 s when the
  vehicle MOVES, minutes-to-rare when parked). A stationary drop-and-leave stick
  wins by PERSISTENCE — over days it will be present for a passing/moving vehicle's
  beacons. No tight dwell budget needed because the stick never leaves.
- **Match a vehicle by SUBSET, not exact set.** 4 sensors/car, and a given visit
  may catch only 1-3. Treat a vehicle identity as ANY overlap in the known
  sensor-ID set, or repeat visits of the same car won't re-link.
- **Contract impact (honest):** the transport handoff is byte-agnostic so it is
  UNCHANGED — but TPMS adds a **new log-line type** (e.g.
  `<secs>,TPMS,<sensorID>,<pressure>,<temp>,<rssi>`) that the deck's collector
  PARSER must learn. Transport frozen; content schema grows by one line type.
  (A stick change; recorded here only because the deck-side parser is the ripple.)

## Status for the NextGen roadmap (honesty ledger update)
- **The "needs a sensor-stick-link protocol" dependency is STRUCK** — it is not
  undesigned. It is a frozen, build-agnostic, CRC-safe external contract (above).
- What remains is IMPLEMENTATION of the collector end on the deck (LoRa via the
  SX1262 blade + BLE via the C6), plus the head overlay that drives it. That is
  build work, not protocol design.
