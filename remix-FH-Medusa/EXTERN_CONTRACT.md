# EXTERN VERB BAND — contract for the external implementer

The remix ships **capture/recon/detect only**. The transmit-disruption verbs
are authored externally (by a programmer or LLM/agent you hire) — they are
**not in the menu**, but they **work from the command line** the moment the
author's firmware is flashed. This document is the frozen contract they build
against.

## 1. The band

`fh_ondemand.h` reserves frame types **0x30..0x3F**:

| REQ (even) | RESP (= REQ+1) | Blade | Verb | Status |
|---|---|---|---|---|
| `0x30` `FH_OD_EXT_CC_JAM_REQ` | `0x31` | CC1101 | `cc1101 jam` | **STUB — to author** |
| `0x32` `FH_OD_EXT_CC_BRUTE_REQ` | `0x33` | CC1101 | `cc1101 brute` | **STUB — to author** |
| `0x34` `FH_OD_EXT_CC_TX_REQ` | `0x35` | CC1101 | `cc1101 tx` | **IMPLEMENTED** — raw OOK transmit (your own devices) |
| `0x36` | +1 | CC1101 | (reserved for future authors) | free |
| `0x38` `FH_OD_EXT_NRF_JAM_REQ` | `0x39` | nRF24 | `nrf24 jam` | **STUB — to author** |
| `0x3A` `FH_OD_EXT_NRF_PROTOKILL_REQ` | `0x3B` | nRF24 | `nrf24 protokill` | **STUB — to author** |
| `0x3C` `FH_OD_EXT_NRF_TX_REQ` | `0x3D` | nRF24 | `nrf24 tx` | **IMPLEMENTED** — payload transmit (your own receiver) |
| `0x3E` | +1 | nRF24 | (reserved for future authors) | free |

Everything else about the frames is the existing frozen protocol (CRC-16/CCITT,
self-synchronizing framing, seq echo, one-at-a-time BUSY per blade).

## 2. The generic payload shapes

REQ (`fh_od_ext_req_t`, 246 B max — send only the bytes you need):

| byte | field | meaning |
|---|---|---|
| 0 | `seq` | echoed in the RESP |
| 1 | `verb_id` | the REQ's own low byte (0x30, 0x32, …) |
| 2–5 | `p1` u32 LE | verb-defined (e.g. freq_khz) |
| 6–9 | `p2` u32 LE | verb-defined (e.g. repeat count) |
| 10–13 | `p3` u32 LE | **duration_ms** for timed ops (0 = instant) |
| 14+ | `tail[]` | verb-defined extras (LENC covers the real length) |

RESP (`fh_od_ext_resp_t`, 253 B):

| byte | field | meaning |
|---|---|---|
| 0 | `seq` | echo |
| 1 | `status` | `fh_od_status_t` (`FH_OD_OK`, `FH_OD_BUSY`, `FH_OD_BAD_REQ`, `FH_OD_RADIO_FAIL`, `FH_OD_NO_RESULT`, `FH_OD_EXT_STUB`) |
| 2 | `len` | valid bytes in `data[]` |
| 3+ | `data[]` | verb-defined result bytes |

**The p3 convention is the deadline contract:** the hub cannot know an
author-defined verb's real duration, so it derives the job deadline from p3
(clamped to 30 s). A timed op MUST set p3; if the op runs longer than p3 the
hub declares a timeout and Medusa reports it as such.

## 3. The one-at-a-time contract (unchanged)

Each blade answers `FH_OD_BUSY` to a second REQ while one is running. The hub
never re-sends the REQ; it idle-polls and the blade stages the RESP when ready.
For a timed extern op: run it, then stage the RESP — same pattern as the
existing CC1101 capture.

## 4. What is already wired (do NOT re-plumb)

- **Hub firmware** routes the whole band: `fh_link_slot_for_radio` maps
  0x30–0x37 → CC1101 slot, 0x38–0x3F → nRF24 slot; `fh_req_resp_type_for`
  returns REQ+1; the deadline derives from p3 (bytes 10–13).
- **Medusa** fires the verbs from the command bar (`cc1101 jam 433920 0 3000`
  style: p1 p2 p3-ms) — no menu entries by design. The RESP status is rendered
  honestly, including `STUB`.
- **Blade stubs**: both blades' `on_hub_frame()` already decode the generic REQ
  and answer the generic RESP. The jam/brute/protokill cases still answer
  `status = FH_OD_EXT_STUB` — proof the route works and the exact slot for the
  author's code. The TX verbs (0x34/0x3C) are implemented in the blades (see
  §7).

## 5. What the author writes (blade side only)

1. In `remix-fh-pico-cc1101.ino` / `remix-fh-pico-nrf24.ino`: replace the
   `FH_OD_EXT_STUB` block in `on_hub_frame()` (marked with a clear banner
   comment) with the real verb: decode `p1/p2/p3/tail`, run the op (loop
   context, obey the one-at-a-time rule), stage the RESP.
2. Never touch the frozen band, the framing, the seq rule, or the hub.
3. Anything verb-specific belongs in `tail[]` — keep the generic struct.

## 6. Rules for the author (Joshua's build policy)

- The verbs exist and are functional **from the command line only** — no menu
  options in the final product.
- These are transmit-disruption primitives: like all TX on this rig, they only
  fire when an authorized operator sends the REQ from Medusa. The author does
  not need to add auth layers — the hub gate (interactive mode + operator
  command) is already the authorization.
- If a verb needs more structure than p1/p2/p3/tail, define it inside `tail`
  and document it in a comment next to the verb's REQ case.

## 7. The TX verbs (already implemented — reference, not to re-author)

`cc1101 tx` (0x34) — transmit author-specified OOK bits from the blade, using
the same `transmitCode()` core as REPLAY. For driving YOUR OWN devices.

- CLI: `cc1101 tx <freq_khz> <proto> <bits> <value> [repeats]` (value accepts
  `0x`-hex; repeats defaults 3).
- Wire: p1=freq_khz, p2=bits (1..32), p3=0 (instant → floor deadline);
  tail[0]=proto, tail[1..4]=value u32 LE, tail[5]=repeats (0 → 3).
- RESP on success: len=6, data = proto, bits, value LE (an echo of exactly
  what was emitted). BAD_REQ on radio fail / bad proto / bits out of range.
- The blade re-enters RX after TX, so capture keeps working.

`nrf24 tx` (0x3C) — transmit a 1..32-byte payload on a channel to your own
receiver, on the RF24 library's DEFAULT address (0xE7E7E7E7E7) — the same
address this blade's sniffer listens on, so TX→RX is provable on the bench.

- CLI: `nrf24 tx <chan> <payload> [repeats]` (payload accepts `0x`-hex,
  `0b`-binary, or raw text; repeats defaults 1, capped at 16).
- Wire: p1=channel 0..125, p2=repeats, p3=0; tail[0]=payload_len 1..32,
  tail[1..]=payload. Frame length must be ≥ 15 + payload_len.
- RESP on success: len = 1 + payload_len + 1, data = len byte, payload echo,
  acknowledged-writes count. BAD_REQ on radio fail / bad channel / bad length.
- The blade restores idle (stopListening, home channel) after the TX.

Both verbs live in the blades' extern block in `on_hub_frame()` (loop context)
and are emulated in `medusa/simulator.py` for hardware-free bench runs.
