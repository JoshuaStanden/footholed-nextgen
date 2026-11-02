# remix-FH-Medusa — FUTURE (planning)

Forward-looking notes for Medusa. The authoritative decisions-and-deferred log is
**`ROADMAP.md`** (captured 2026-10-22, a settled-decisions record kept verbatim so
we don't re-litigate) — this file is the short planning pointer on top of it.
Read `ROADMAP.md` for the full rationale on each item below.

## Deferred (talked about, not built — see ROADMAP)
- **Signal-visualization code (1-D time strip).** The agreed "shape" view is a
  crude signal-present/OOK-high-low timeline over time, NOT a 2-D waterfall. Two
  renderers: a tiny glyph on the field device, a roomier (still rough) strip in
  Medusa. Lossy is fine — the raw dump file is the record, the viz is convenience.
- **DSP feature extraction** (the cheap on-device "what shape was it" summary).
- **The interpretation/LLM layer** ("Medusa, anything significant?") — a
  HOST-side bolt-on fed the dump + feature summary. Never on-device (no ESP runs
  an LLM; decision #3).
- **2-D waterfall** — only if a real frequency-hopping case demands it.
- **The dump file format** (replayable AND viz-able) — specify when the CC1101
  capture path is real on hardware.

## Hardware gap (needs a new radio, not code)
- **125 kHz LF RFID** — none of the four radios reach it (PN532 is 13.56 MHz).
  Adding it is a 5th radio with its own LF front-end + coil; it slots into the
  architecture as another on-demand radio (`lf_read`/`lf_write` Layer 1, a
  `badge.*` LF branch Layer 2) and a Gen2 one-radio blade. Decision: not added
  now; NFC/badge intents stay 13.56 MHz.

## Blocked on firmware, not Medusa
- The radio-blade on-demand round trips (cc1101/nrf24/sx1262) are proven in the
  simulator but hardware-pending. If a blade's hub link can't stream (the
  arduino-pico `SPISlave1` transmit risk), that is a BLADE fix (the pico-sdk
  port, `../remix-PICO-SDK-PORT-PLAN.md`), not a Medusa change — Medusa sees only
  the frozen frames over USB and does not change.
- Not-built Layer-2 intents (nfc_write, nfc_dump, fetch_dump, tracker_scan,
  lora_listen, radio_test) each raise `NotImplementedError` naming the missing
  Layer-1 atomic; swap the raise for the real call when that atomic ships (the
  composition shape above it already shows how to wire it).

## Still the one real dependency (RESOLVED 2026-10-31)
The hub mirrors frames out its USB console and routes the on-demand REQ/RESP
flights — built, simulator-gated green, hardware compile pending. Medusa's
read-side was always closest to working; the command verbs close the round trip
through that hub behavior.
