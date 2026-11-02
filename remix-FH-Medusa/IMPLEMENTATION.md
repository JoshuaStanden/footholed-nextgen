# FH_Medusa — IMPLEMENTATION notes (for future Kiro / future Joshua)

This is the honest handoff: what is proven, what is ASSUMED and must be checked
against real hardware, the open items, and how to extend it safely.

## Proven by running (host side)
`selftest.py` passes with plain `python3`, no hardware, no pyserial:
- `protocol.load()` parses the vendored C headers and the **drift guard** passes
  (every parsed struct size equals the header's `_Static_assert`).
- CRC-16/CCITT-FALSE known-answer `0x29B1`.
- Frame encode → resync-safe decode → struct unpack round-trips (GPS, heartbeat,
  capture-with-mac).
- Simulated REQ/RESP matches by seq; the **BUSY lock** rejects a concurrent request.

That proves Medusa's logic. It does NOT prove the firmware emits these bytes —
that's the other half, pending hardware.

## ASSUMPTIONS that must be verified on real hardware (the risk list)
1. **The hub forwards node frames OUT the USB console, verbatim.** Medusa assumes
   the hub's USB-serial stream carries the raw `fh_protocol` frames (captures,
   GPS, heartbeats) and that on-demand RESP frames from the Pico reach USB too.
   The current hub firmware logs to SD and drives the aggregator; whether it also
   *mirrors frames to the USB console* is a HUB firmware behavior that may need to
   be added. **If Medusa sees nothing, this is the first thing to check** — the
   hub may need a "tee every received/sent frame to the console UART" path.
   (This is the one real firmware dependency. It's small, but it's not free.)
2. **Who owns the USB console — frames or human-readable logs?** The hub's console
   today prints status text (board init, UI status line). Medusa expects *binary
   frames*. Decide the contract: a dedicated binary frame mirror, OR a framed
   channel distinct from the text log. Mixing human text + binary frames on one
   stream works (the decoder resyncs past non-SYNC bytes) but is messy — a clean
   split is better. **This is a design decision to make with the hub, not in Medusa.**
3. **Console baud.** RESOLVED 2026-10-30: the P4-WIFI6-DB's USB-C is a CH343
   USB-UART on UART0 (not USB-Serial-JTAG), so baud MATTERS. The hub is now set
   to 921600 to match Medusa's fixed default. (Items 1 and 2 are also resolved:
   the hub bridge mirrors frames when ATTACHed and suppresses text logs.)
4. **On-demand commands originate at the hub, not the laptop.** Today the hub is
   the thing that PROMPTS the Pico (fh_ondemand REQ). For Medusa to issue them,
   either (a) the hub relays Medusa's REQ frames through to the Pico, or (b)
   Medusa's commands are addressed to the hub which re-issues them. **Pick one.**
   Option (a) keeps the hub a transparent router (simplest); option (b) lets the
   hub arbitrate. Until this is decided + implemented in hub firmware, Medusa's
   command verbs (ping/sample_*/nfc/lora_tx) are UNPROVEN end-to-end — the read
   side (events) is the part that works first.

## Open items (not done, deliberately)
- `gps()` returns the last fix seen from the event stream. There is no explicit
  "poll GPS now" verb because GPS already streams unsolicited (0x02). If you want
  a synchronous poll, add a hub verb; don't fake it.
- No persistence/logging in Medusa yet (it's a live view). Adding a "tee to a
  file" is easy and non-invasive if you want a host-side capture log.
- The TUI is curses/stdlib and minimal by design. A richer UI (textual, a web
  view) is just another client on the same `MedusaClient` — don't put protocol
  logic in it.
- LoRa listen (0x2B/0x2C) and raw LoRa RX indication (0x2A) are parsed but have
  no verb/handler surfaced in the client beyond `on_lora_rx`. Add if needed.

## How to extend (do it this way)
**Add a new on-demand verb:** it already exists in `fh_ondemand.h` → add a method
to `MedusaClient` that packs the `*_req_t` struct and calls `self._req(REQ_NAME,
payload, RESP_NAME, timeout)`. No codec/protocol changes — the structs are
already parsed. See `sample_nrf24` as the template.

**The protocol changed in firmware:** re-copy BOTH `fh_frame.h` and
`fh_ondemand.h` from a firmware zip's `components/fh_protocol/include/` into
`protocol/` here, then run `selftest.py`. If a struct changed size, the drift
guard tells you exactly which one. If the parser hits a C construct it doesn't
model, it raises `DriftError` naming the field — extend `medusa/protocol.py`'s
`_CTYPE` / field regex, never hand-patch a layout.

**Keep the layering clean:** protocol (layouts) ← codec (bytes) ← session
(transport + routing + BUSY) ← client (verbs + events) ← TUI (render). Each layer
only imports downward. The TUI must hold zero protocol knowledge.

## Why this exists as its own experimental zip
Per the owner's instruction: anything Medusa is experimental and isolated. It
reads the frozen protocol but changes nothing in the firmware zips or the system
map. If it proves out on hardware, it can later be promoted; until then it's a
clearly-labeled sidecar that can be deleted with zero impact on the device.
