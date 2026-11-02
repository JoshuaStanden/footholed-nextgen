# AGENTS.md — remix-FH-Medusa (for AI / LLM agents)

Read this BEFORE editing Medusa. The repo-root `../AGENTS.md` governs the whole
rig; this file carries the Medusa-specific lore. Medusa is **self-contained,
host-side, and touches no firmware** — deleting this folder leaves the device
unaffected. Five-doc spine: WIRING / README / AGENTS (this) / CONTINUITY /
FUTURE, PLUS the design docs (API_DESIGN, TUI_DESIGN, SIMULATOR, EXTERN_CONTRACT,
IMPLEMENTATION) — do not delete those; they are distinct design records.

## Non-negotiables (this package)
1. **NEVER hand-copy a wire layout into Python.** `medusa/protocol.py` PARSES the
   vendored C headers in `protocol/` at import and derives every struct from
   them, then checks each parsed size against the header's own `_Static_assert`
   and raises `DriftError` if they disagree. Drift is structurally impossible BY
   THIS DESIGN — do not add a hand-written struct offset. To track a firmware
   protocol change: re-copy `fh_frame.h` + `fh_ondemand.h` into `protocol/`
   (byte-identical from any firmware package) and re-run `selftest.py`.
2. **The codec mirrors `fh_frame` exactly** (SYNC A5 5A / TYPE / LEN / PAYLOAD /
   CRC-16/CCITT-FALSE over TYPE..PAYLOAD, little-endian, resync-safe). The
   known-answer is `CRC16("123456789") == 0x29B1`. Do not "optimize" the CRC.
3. **Layer 1 vs Layer 2.** Layer 1 (the hub's atomics, mirrored here as client
   verbs) is the COMPLETE, NEUTRAL hardware surface — transmit included.
   `medusa/intents.py` is Layer 2: legitimate COMPOSITIONS only. A not-built
   atomic raises a clear `NotImplementedError` naming the missing piece, never a
   silent stub. Do not add "bad" functions at Layer 1; do not ship offensive
   Layer-2 intents.
4. **Active verbs go through the `confirm()` gate** (default DENY) — nothing that
   transmits/writes fires without an explicit yes. This is an operational-
   consequence guard, not a suspicion gate; keep it.
5. **Medusa talks to the HUB, never to a blade directly** (ROADMAP decision #1).
   The hub owns the one-at-a-time BUSY arbitration; a second on-demand verb while
   one runs raises `medusa.session.Busy`, mirroring the firmware.
6. **Touch no firmware.** Medusa consumes the frozen protocol; it modifies no
   `.ino`/`.c`/`.h` in any other package. Keep it self-contained.

## Status reality
The HOST side is proven by running (`selftest.py`: drift guard, CRC KAT, event
decode, REQ/RESP, BUSY lock — all pass, simulator-gated). The FIRMWARE side is
proven on hardware only for BLE/boot + the GPS blade; the radio-blade round trips
are hardware-pending. So "Medusa works" means Medusa is unlikely to be the cause
when a bench thing fails — not that the end-to-end radio path is proven.

## Review discipline
Subject to `../remix-MULTI-CHECK.md`. The drift guard is the structural proof;
`selftest.py` is the executable one — run it after any protocol-touching change.
Docs travel with the edit.
