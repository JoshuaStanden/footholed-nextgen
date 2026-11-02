# FH_Medusa — ROADMAP / decisions log

> Captured 2026-10-22 from a design conversation. These are DECISIONS and
> DEFERRED ideas, not committed work. Nothing here is built unless it also
> appears in the code + selftest. Future Joshua / future Kiro: this is so we
> don't re-litigate things we already settled.

## Decided (direction we agreed on)

1. **The hub is the ARBITER, not a pass-through.** Medusa talks to the HUB; the
   hub owns the Pico and the one-at-a-time BUSY arbitration. Medusa does NOT
   address the Pico directly — that was judged too complicated and risks Medusa
   fighting the hub for a radio. (This confirms IMPLEMENTATION.md risk #4 as
   "option (b): the hub arbitrates.")

2. **Gen2: the ESP32-P4 head inherits the hub's arbiter role, heavier.** In Gen2
   the P4 does the heavy lifting + communication; the blades/devices stay dumb
   and light. The hub→P4 path is "same arbiter role, more capable brain." Medusa's
   client logic is effectively a prototype of what the P4 head will do on-device.

3. **NO on-device AI.** In the field the device is a SENSOR + LOGGER + basic
   status display only. It is not smart alone. All interpretation (incl. any
   future LLM-style "is there anything significant in this dump?") lives where
   Medusa runs — laptop now, phone/cloud later. Rationale: no ESP runs a language
   model; cramming intelligence on-device adds burden for no field benefit, and
   the device must stay dumb-fast-reliable. On-device, at most, is cheap DSP
   feature extraction (e.g. "periodic burst at 433.92, 95% duty") — NOT reasoning.
   - Hardware note for the record: ESP32-S3 (hub + XIAO-S3) has SIMD + ESP-DL/
     ESP-NN for TINY ML; ESP32-P4 is stronger CPU+SIMD but still no NPU and no LLM.
     The "what is this signal" knowledge problem is a signature DB / decoder job
     (cf. rtl_433), not AI — and it belongs in Medusa, not the device.

4. **The device GUI and the Medusa TUI are the SAME menu tree, two renderers.**
   Build the menu/command logic once (with the TUI); the field handheld's GUI is
   a "light version" of it. The screen is a COMMAND LAUNCHER + confirmations, not
   a data visualizer. Example: "sniff 5 GHz" shows a live list; "sniff 15 s of
   CC1101" just says "done — dumped to file, have at ye." The SD file (or Medusa
   on the laptop) is where the real data lives. This keeps the deferred LCD a
   SMALL job whenever it's tackled.

5. **Dumping raw capture to file is CRITICAL and non-negotiable; visualization is
   convenience.** If the viz is wrong/missing you still have the data; if the dump
   is wrong, nothing else matters. The viz is allowed to be LOSSY/rough because it
   is NOT the record — the file is the truth. (Same "prove the boring core first"
   discipline as the rest of the project.)

6. **Primary "shape" view = a 1-D time strip (signal present/absent or amplitude
   over time)**, not a spectrum/waterfall. A human eye catches "repeating burst /
   stuck key / beacon / anything here at all" from a crude on/off timeline. Two
   renderers again: the field device draws a tiny crude glyph/sparkline; Medusa
   draws a roomier (still rough) strip on the laptop. 2-D waterfall (freq × time)
   is deferred — only earn it if a real frequency-hopping case demands it.

## Hardware gaps (need new radio before the capability can exist)

- **125 kHz LF RFID — WE HAVE TO ADD ANOTHER RADIO.** None of the four onboard
  radios can reach 125 kHz: PN532 is 13.56 MHz only, CC1101 floors around 300 MHz,
  nRF24 is 2.4 GHz, SX1262 is ~150–960 MHz. 125 kHz (HID Prox, EM4100, Indala,
  T5577) needs a DEDICATED LF front-end — an LF reader chip + its own tuned
  antenna coil. This is a physics wall, not firmware.
  - To READ 125 kHz: an RDM6300 or EM4095-based module.
  - To READ **and WRITE** (e.g. program T5577 blanks): a proper LF reader/writer stage.
  - This is the one capability the Flipper Zero has that we don't (it carries a
    separate LF stage + coil); we out-cover it on 2.4 GHz, dual-band Wi-Fi, and LoRa.
  - Fits our architecture cleanly: the LF reader is just ANOTHER on-demand radio
    (`lf_read` / `lf_write` atomics in Layer 1; a `badge.*` HF/LF branch in Layer 2),
    and in Gen2 it is a natural one-radio BLADE.
  - DECISION: not added to FH-Advanced now. NFC/badge intents are scoped to
    13.56 MHz (PN532) until an LF radio is added. "badge write billybob" covers
    HF credentials today; 125 kHz prox needs this 5th radio first.
  - SUPERSEDED 2026-11-01: ALL card-reader radios are now PARKED — the PN532
    (HF) as well as LF. Dedicated read/write/copy/emulate tools already exist,
    so FootHoled does not rebuild them. Hooks stay reserved; see
    ../remix-PARKED-CARD-READER-RADIOS.md.

## Deferred (talked about, not now)
- The actual signal-visualization code (1-D strip) in Medusa and on-device.
- DSP feature extraction on-device (the cheap "what shape was it" summary).
- The LLM/interpretation layer in Medusa (the "Medusa, anything significant?"
  answer) — a HOST-side bolt-on, feeding it the dump + feature summary.
- 2-D waterfall view (only if needed).
- The dump file format (should be both replayable AND viz-able later) — not yet
  specified; decide when the Pico's CC1101 capture path is real on hardware.

## Still the #1 real dependency (from IMPLEMENTATION.md)
> RESOLVED 2026-10-31: the P4 hub firmware mirrors frames and routes the
> on-demand REQ/RESP flights (Medusa + hub + blades written; simulator-gated
> green; hardware compile pending Joshua). The note below is the historical
> rationale.
The hub must MIRROR fh_protocol frames out its USB console (and relay Medusa's
on-demand commands to the Pico, per decision #1). Until that small hub-firmware
behavior exists, Medusa's read-side (events) is closest to working and the command
verbs are unproven end-to-end. This is a hub-firmware change — NOT a Medusa change
— and is intentionally not made here (Medusa touches no other zip).

## Status snapshot (2026-10-31) — what shipped since this doc was captured
- Tiers 1–2 BUILT: protocol drift guard, hub arbitration contract, CC1101
  capture/replay/raw-capture-to-dump, nRF24 scan/sniff capture-to-dump
  (DUMP_IND chunk streaming), all simulator-gated green.
- Tier 3 BUILT (in-project scope): the extern verb band 0x30–0x3F is routed by
  the hub generically; `cc1101 tx` (0x34) and `nrf24 tx` (0x3C) are implemented
  in the blades (TX to the operator's OWN devices); `cc1101 replay <freq>
  <seq>` replays a SPECIFIC capture from Medusa's decode-triple store.
- Still EXTERNAL stubs by policy: `cc1101 jam` (0x30), `cc1101 brute` (0x32),
  `nrf24 jam` (0x38), `nrf24 protokill` (0x3A) — the disruption verbs are
  deliberately left for an external author (frozen contract:
  EXTERN_CONTRACT.md; hub CONTINUITY.md §5b). wifi/bt/sigint disruption verbs
  have no protocol contract at all yet.
- See Medusa CONTINUITY.md for the running session log.
