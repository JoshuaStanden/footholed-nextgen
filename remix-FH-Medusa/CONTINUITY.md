# remix-FH-Medusa — CONTINUITY (future Kiro + Joshua)

> Resume if a session drops. Snapshot 2026-10-25. Folder `~/Desktop/remix footholed/`.
> Standing rules in the P4 hub CONTINUITY.md.

## What this is
Medusa — the host-side (laptop) Python "face" that drives the hub over USB. The
operator console for the headless hub. Carried from `FH-Advanced-Medusa-EXPERIMENTAL`.

## Pass 1 — the reconcile finding (why the FIRST pass had almost no diff)
NOTE: superseded by Pass 2 below, which DID change functional code. This section
was accurate for the topology reconcile (2026-10-25 AM): reconciling Medusa for
the REMIX P4/SPI world required **no functional code changes**. Reasons, verified:
1. **Medusa↔hub is USB, and that did not change.** The P4 hub keeps its
   USB bridge. `session.py` opening a serial port is still correct.
   (CORRECTED 2026-10-30: the USB-C is a CH343 USB-UART on UART0 @921600, not
   USB-Serial-JTAG — see FIRST HARDWARE RUN below.)
2. **The wire contract is byte-identical.** `protocol/fh_frame.h` +
   `fh_ondemand.h` were re-vendored from the frozen headers (md5 matches the
   hub). The drift-proof parser (`protocol.py`) parses those headers, so Medusa
   derives the exact same layouts — no hand-copied offsets to update.
3. **No hard-coded topology in the code.** Nodes are referenced by `src_id`
   (the frozen wire value) and rendered generically as `node{src_id}`. There was
   no "3 UART" / node-name map / hub-board string to fix.
4. The UART→SPI change was INTERNAL to the hub (hub↔peers/blades). Medusa never
   saw it — it only ever saw the hub's USB stream.

So: **Medusa works against the P4 hub unchanged.** The only edits were
doc/comment wording that described the pre-REMIX topology (would have lied to
the next reader otherwise):
- `README.md` — "What it is" paragraph rewritten to the P4/SPI topology + an
  explicit "what did NOT change for Medusa" note; banner line (S3→P4 hub);
  system name (FH-Advanced→FootHoled-REMIX).
- `medusa/simulator.py` line 126 — comment "the Pico responds" → "the radio
  blade responds" (comment only; the simulated BYTES are unchanged and correct).
- `SIMULATOR.md` — same "Pico"→"radio blade" wording.

Line-count check (Pass 1 only): at that point every .py was identical to source
except the one comment — the working code was not yet touched. Pass 2 changed
this (tui.py + tui_textual.py now diverge functionally — see below).

## Pass 2 — status-bar redesign (2026-10-25 PM, the "fixing Medusa" work)
Walked the top status bar with Joshua piece-by-piece and rebuilt it. The old bar
(`FH_Medusa [ATTACHED] nodes:N good:N crc:N HH:MM:SS`) was replaced by six
color-coded health cells. Full design + rationale: `TUI_DESIGN.md` →
"STATUS BAR (REMIX redesign)". What changed in CODE:

- **`tui.py` (headless model = single source of truth):**
  - new `StatusCell(text, state)` (state ∈ ok/warn/bad/idle).
  - model gained status state the old model discarded: `_last_seen[src_id]`
    (monotonic per-src — client.nodes never forgets, so we track freshness
    ourselves), `_last_good_t`, `_last_gps`+`_last_gps_t`, and a crc-RATE sampler
    (`_crc_sample`: diffs the cumulative codec counter over `CRC_WIN_S`).
  - event handlers (`_on_capture/_on_hb/_on_gps/_on_summary`) now stamp those.
  - **`status_bar()`** computes the 6 cells; BOTH renderers consume it (no
    logic duplicated across the two TUIs).
  - curses `run_view` got green/amber/red color pairs (graceful no-color
    fallback) and draws per-cell with `│` group separators; degraded states add
    text without reflowing slots.
  - `_selftest_model` extended to assert the bar against the simulator.
- **`tui_textual.py`:** `_refresh` renders the same `status_bar()` cells as Rich
  markup; `#status` CSS bg `$accent`→`$panel` (cells color their own text);
  removed the now-unused `import time` (its only use was the deleted clock).

The six cells (see TUI_DESIGN.md for the authoritative spec):
  1 Medusa (hub link: green=attached+flowing) · 2 Display (head src_id 4:
  green=hub-confirmed bidir; RED until head firmware PONGs) · 3 X-S3 (src2) ·
  4 X-C5 (src1) · 5 GPS (green=fix+sats / amber=acquiring / red=dead, three-tier
  off `fh_gps_fix_t.fix_qual`) · 6 link (USB integrity; quiet green, loud
  `crc/s` when rising — integrity only, silence ≠ corruption).

Enabler done on the HUB side (not Medusa): `FH_SRC_HEAD=4` added to the frozen
enum (all 8 copies, md5-verified) + hub health node 4 wired + head peer given
its src_id so the hub PINGs it — see hub CONTINUITY §3b. Medusa reads head
liveness as node-4 freshness relayed in the hub stream.

VERIFIED (on-machine, no hardware): `py_compile` clean on both TUIs + medusa/*;
`python3 tui.py --selftest-model` ran the model against the SIMULATOR and
`status_bar()` returned `['Medusa','Display','X-S3','X-C5','GPS●11','link']` with
Display=red (sim sends no src4 — honest), peers+GPS green. Proven-by-EXECUTION
for the logic. The curses color draw + Textual markup are proven-by-reading +
compile only (neither TUI is drivable headless here).
OPEN / bench: GPS **amber** (acquiring) is implemented but NOT exercised — the
sim only sends `fix_qual=1`. Verify on the bench that the real GPS blade emits a
`fix_qual=0` frame while unfixed (else amber collapses to red; small blade-fw
touch). Display green needs head firmware (hub CONTINUITY §4.1).

## Pass 3 — live-stream zone, below the status bar (2026-10-25 PM)
Walking the zones below the status bar, issue-by-issue. Issue 1 (DONE): the live
stream was too short AND the two renderers disagreed (curses hard-coded
`stream[-2:]` = 2 lines; Textual `Log` height 4). Joshua wants ≥5, proportional.
- **curses (`tui.py` draw):** stream height is now `stream_h = max(5, (h-3)//3)`
  — ~1/3 of the usable height, floor 5. The column HEADER row and first content
  row are derived from it (`hdr_row = 1+stream_h`, `top = hdr_row+1`) instead of
  the old hard-coded rows 3/4 — so growing the stream pushes the device list /
  detail down correctly. All three former literal `3`/`top=4` sites updated.
- **Textual (`tui_textual.py`):** `#stream` → `height: 1fr; min-height: 5`
  (proportional, floored); the content Horizontal got id `#body` with
  `height: 2fr`, so stream:body ≈ 1:2 (~1/3 stream). Pure CSS + one id.
VERIFIED (Issue 1): syntax check (py_compile) clean; model self-test still green.
NOTE: "syntax check", NOT compiling — no build/flash/toolchain runs on this
machine (Joshua's absolute rule). Layout math reasoned by reading (curses not
drivable headless): 24-row term → 7 stream lines; short 10-row → floored to 5;
tall 50-row → 15. `if y >= bottom: break` guards overdraw on tiny terminals.

Issue 2 (DONE): the stream printed raw `src=N`; now it speaks node names.
- Single source of truth: `MedusaModel.src_name(src_id)` classmethod with map
  `{1:"x-c5(5)", 2:"x-s3(2.4)", 3:"pico"}`, fallback `src{N}`.
- Used in: capture stream line, capture device-row label + detail, heartbeat
  node-row label + detail, summary stream line. (The dedup dict KEY stays
  `node{src_id}` — internal, never rendered.)
- `display` (head, src_id 4) deliberately NOT in the map: the head is a render
  sink, emits no capture/summary, so it never appears in rolling events. It
  keeps its STATUS-BAR liveness light (different concern). A head HEARTBEAT, if
  one ever arrives, falls back to `src4` in the node row — acceptable/honest.
- BT (BLE) is an ORDINARY capture source, src_id 5 (`FH_SRC_WS_BLE`, added to
  the frozen enum this session — all 8 copies, md5 re-verified identical). It is
  hub-LOCAL (the P4's on-board BLE radio) but streams/logs like any other
  source: `src_name(5)="BT"`, rendered `CAP BT …`. Correction to an earlier note
  here: BLE is NOT a special category and needs no separate event path — I was
  wrong about that. The only pending piece is hub FIRMWARE enabling its BLE
  observer + stamping captures with src 5 (DONE 2026-10-30); the Medusa naming/rendering
  is done, and the simulator now emits src-5 BLE so BT is exercised.
- Stream verbs shortened to save width: `CAPTURE`→`CAP`, `SUMMARY`→`SUM`.
  Example line: `CAP x-s3(2.4) a4:… rssi=-52` / `CAP BT -44 …` / `SUM pico …`.
Issue 4 (DECIDED — no code change): heartbeats do NOT tick in the stream.
Rationale (Joshua): heartbeats (src 1/2/3, soon 4) are the SAME liveness frames
that drive the status-bar health lights — streaming them would be redundant with
the bar AND would push real capture events off the ~5 visible lines every ~2 s.
This is already the behavior (`_on_hb` updates the device row + health state, no
`_push_stream`), now recorded so nobody "restores" heartbeat lines later. Holds
for ALL sources (ESP32 and blades). Blade stream representation is handled by
Issue 5 below (by-radio, by-action) — that WAS Joshua's "simplify blades" idea.

STILL OPEN in this zone: optional filter/color to tame the firehose. BT naming
is DONE; the hub BLE observer is DONE too (2026-10-30, live on hardware).

Issue 5 (DONE): blade results stream by RADIO name, not "pico". Joshua's model:
Pico radio functions are FIRED actions, not a passive feed — so a blade event is
a completed action with a per-ACTION summary, named by the radio that acted.
- New shared `MedusaModel.fire_result(radio, summary, detail)` — pushes ONE
  stream line `CAP {radio} — {summary}` AND the ★ detail row. Both TUIs call it
  (refactored `tui.py run_command` + `tui_textual.py _run_command` cc1101/nrf24/
  nfc branches off `add_command_result`), so the two stream lines can't diverge.
- Summary is per-action: DURATION for a timed capture (`CAP CC1101 — 15 SEC`),
  OUTCOME for a one-shot function (`CAP NFC — MIFARE 1K`; later `… — REPLAYED`).
  CC1101/nRF24 pass a dwell/scope summary today; NFC passes the card kind.
- `pico` dropped from `src_name` (src 3 only heartbeats, never streams).
- `ping` stays an `add_command_result` (status query, not a fired capture).
- VERIFIED: syntax check + self-test green; a fired cc1101 renders
  `CAP CC1101 — 3 SEC`, and the test asserts no `pico` leaks into the stream.
- NOTE: this by-radio / by-action display IS Joshua's "simplify blades" idea —
  it is implemented here, not pending. (Earlier notes calling it a future idea
  were stale and have been corrected.)

Issue 3 (DONE): dropped the per-line `HH:MM:SS` from the stream. The stream IS
"now" — newest line = current moment, order = sequence — so a clock per line was
~9 wasted cols in a narrow firehose and (at HH:MM granularity) wouldn't even
change across the visible window. `_push_stream` now appends the bare line.
NOT touched: the SD log keeps full timestamps (HUB-written, GPS-disciplined —
Medusa's display clock was never the log source), and `import time` stays (still
used for row first/last, monotonic freshness, sleep). VERIFIED: syntax check +
self-test green; sample line now `CAP BT 5c:9a:3e:11:77:b0 rssi=-54` (no clock).

Issue 6 (DONE): color-by-source in the stream (NO filtering — Joshua explicitly
does not want anything muted). Palette (black bg): BT=blue, x-s3(2.4)=green_light
(bright green), x-c5(5)=green_dark (plain green), GPS=yellow, SUM=white, fired
radio ops (CC1101/nRF24/NFC)=orange. Impl: `_push_stream(line, color)` now stores
(text, color) TUPLES; color chosen at creation via `src_color(src_id)` map +
explicit tokens (GPS yellow, SUM default white, fire_result orange). Renderers:
curses added color pairs 4-6 + a `_sc(token)` attr map (orange=256-color 208 or
yellow+bold fallback; green_light=green+BOLD); Textual switched `Log`→`RichLog`
(markup) with a token→Rich-style map (orange=rgb(255,135,0)). The stream-shape
change (str→tuple) rippled into the self-test — asserts updated to unpack
`(t, c)` and now also verify the palette. VERIFIED: syntax check + self-test
green (BT blue, GPS yellow, fired op orange, 2.4 green_light all asserted).

## Pass 4 — left pane (interaction column) selection fix (2026-10-25 PM)
Bug Joshua hit on hardware: as new rows stream in, the highlighted row drifted
up and reaching/selecting the NEWEST row by arrowing was "an effort in futility"
— the bottom kept receding. Root cause: `sel` was a raw integer index; new rows
append at the end, so the cursor couldn't catch the growing bottom, and the
highlight appeared to slide up the (top-drawn) list.
FIX (both requested): (A) IDENTITY-STICKY selection + (B) follow-newest.
- Model: replaced `self.sel:int` with `_sel_key` (selected row's KEY) + `_follow`
  (default True). New methods: `sel_index()` (resolves key→current index;
  follows newest when armed), `move(d)` (↑/↓ → disengage follow, step sticky key
  by identity), `jump_newest()` (re-arm follow, snap to last). `add_command_result`
  now pins the ★ result by key + stops following (focus follows intent).
- curses: highlight compares `r.key == _sel_key` (robust vs list timing); header
  shows `▶following`; `End`/`G` → `jump_newest()`.
- Textual: table rebuild moves the cursor to `sel_index()` after re-adding rows;
  `on_data_table_row_highlighted` only disengages follow for USER changes (a
  `_rebuilding` guard suppresses the programmatic rebuild's events); `end` key →
  `action_newest`; NOW line shows `▶following`.
- VERIFIED: syntax check + self-test green, incl. new asserts — follow on by
  default, ↑ disengages, a new row does NOT slide the sticky selection off,
  `jump_newest` re-arms, firing a command pins (follow off).

## Pass 5 — command-bar verb grammar (2026-10-25 PM)
Joshua's model: the command bar is `<module> <verb> <args>` — e.g.
`cc1101 cap 11sec`, `cc1101 replay whatever.sub`, `nrf24 scan 0 80`. Replaced the
old flat `<module> <positional-numbers>` parser.
- Protocol (frozen, additive): the CC1101_REQ `action` field (the only radio
  with one) got NAMED constants in `fh_ondemand.h` — `FH_OD_CC1101_ACT_LISTEN=0,
  RX_ONE=1, RAW_OOK=2, REPLAY=3` — all 8 copies, md5 re-verified identical. This
  is the per-radio verb mechanism the protocol author already set up (the old
  `...` comment). nrf24/nfc have NO action field, so their extra verbs need a
  (bigger) struct add later — NOT done.
- Medusa (one shared parser, both TUIs): `MedusaModel.parse_command(line)` →
  tagged result; `run_command(line)` routes it to fire_result/add_command_result/
  NOW. Human units via `_dur_ms` (`11sec`/`500ms`/`2s`/bare=ms). `_VERBS` table
  marks each verb live|reserved. LIVE verbs map to existing library calls
  (cc1101 cap→sample_cc1101, nrf24 scan→sample_nrf24, nfc read→read_nfc,
  lora tx→lora_tx, ping). RESERVED verbs (replay/raw/sniff/emulate/write) parse
  and return "recognized but NOT implemented yet" — not faked, not a cryptic
  error. Both TUIs' command handlers are now thin wrappers over run_command.
- VERIFIED: syntax check + self-test green — `cc1101 cap 433920 11sec` fires
  "11 SEC", `replay` returns reserved, `nrf24 scan 0 80` fires, unknown module/
  verb give clean errors, `_dur_ms` units all parse.
- FIRMWARE TODO (to light reserved verbs): hub SD→blade TX path + blade TX for
  REPLAY; action fields on nrf24/nfc REQ for their extra verbs.

## Pass 6 — DETAIL (right) pane rework (2026-10-25 PM)
Joshua: the right pane should reflect the highlighted left item as clean
metadata (we observe announcements/presence, not payload), and fired ops should
reiterate the command + that the full duration recorded — NOT a raw struct dump.
- Per-type records built in the model (decoders added: `wifi_ftype`, `band_chan`,
  `src_color` reuse, `_stamp`, `_od_status`):
  - Wi-Fi (`_on_capture`, src 1/2): time·mac·frame(decoded)·band+ch·rssi·seen.
  - BT (`_on_capture`, src 5): time·addr·rssi·caps·mfr·seen — caps/mfr are honest
    PLACEHOLDERS until real BLE adv bytes flow — now REAL (2026-10-30, `_ble_meta`).
  - GPS (`_on_gps`, new dedup row key "gps"): time·lat·lon·sats·fix.
  - Fired op (`parse_command` live branches): echoes the exact command string +
    `recorded Ns (full Nms window)` + a one-line `_od_status(r)` result. The raw
    RESP struct dump (`f"...\n{r}"`) is GONE.
- VERIFIED: syntax check + self-test green, incl. new asserts — fired-op detail
  echoes the command + recorded duration, Wi-Fi detail carries a decoded frame
  type, and NO row detail contains a struct repr (the assert even caught a dirty
  test fixture, now routed through the real run_command path).
- DONE 2026-10-30: BT caps/mfr decode (hub BLE observer + `extra[10]`); richer device
  stats/history (RSSI trend, per-channel) is a future enhancement, not built.

## Pass 7 — HALEHOUND verb surface + build policy (2026-10-25 PM)
Joshua supplied the full HALEHOUND-CYD capability menu and the rule: EVERY verb
goes in the command grammar (so the vocabulary is complete and there are labeled
seams), but the assistant only AUTHORS the capture/recon/detection primitives.
Transmit-disruption primitives (the actual frame transmitters) are present as
recognized verbs but NOT built here — an external builder wires them.
- Three verb states in `_VERBS`:
  - `live`    — wired to a real library call now (cc1101 cap, nrf24 scan, nfc
    read, lora tx/listen, ping).
  - `reserved`— OBSERVATIONAL/capture/detect verb, recognized, pending firmware:
    wifi monitor/scan/probe-sniff/station-scan/dupe-ssid/deauth-WATCH/
    reconnect-test; bt sniff/scan/airtag-detect/stalkerware-detect/
    whisperpair-scan; nrf24 sniff/spectrum; cc1101 replay/raw/spectrum;
    sigint eapol/pmkid/wardrive/saved; tools serial/fwupdate/touch-cal/gps/
    radiotest. (EAPOL/PMKID classified as CAPTURE/recon — cracking is off-device
    and out of scope; the device only captures.)
  - `extern`  — verb recognized, implementation left to an external module (the
    transmit-disruption set): wifi deauth/beacon-spam/auth-flood/evil-twin/
    captive-portal; bt beacon/jam/spoof/pair-spam; nrf24 jam/protokill; cc1101
    jam/brute. parse_command returns a clear "implementation not in this build
    (external module)" — no crash, no attack scaffolding.
- POLICY NOTE for future sessions: do NOT re-litigate this with Joshua by asking
  him to justify a verb's use and then refusing anyway — that pattern was called
  out and stopped. The split is by WHAT THE CODE DOES: observe/capture/detect =
  build; transmit-to-disrupt primitive = verb present but authored externally.
- VERIFIED: syntax check + self-test green — eapol/wardrive/deauth-watch report
  "capture/recon" (reserved), deauth/bt-beacon report "external module" (extern),
  live verbs still fire, unknown module/verb still clean.
- Note: `wifi`/`bt`/`sigint`/`tools` are command-bar MODULES in the verb table;
  they are not new wire src_ids. Only the live verbs hit the wire today.

## Pass 8 — host-side recon analytics wired REAL (2026-10-25 PM)
Built five reserved verbs into REAL functions where the data already flows to
Medusa — no new wire opcodes (these analyze captures/GPS the sniffers already
stream). New client library methods + a bounded observation store:
- `MedusaClient._obs` (MAC→{src,count,rssi,rssi_max,first,last,...}, cap 2048) +
  `_wardrive` sightings list (cap 4096), both fed by `_observe(cap)` called from
  the capture dispatch. `_observe` also logs a GPS-tagged sighting when a valid
  fix is held.
- Library verbs: `scan_wifi()` (observed emitters newest-first), `scan_stations()`
  (same store; true AP/station split needs richer 802.11 fields not in extra[]),
  `detect_dupe_ssid()` (UPGRADED to EXACT in Pass 9 — see below), and
  `wardrive_log()` (capture↔GPS correlation). `gps()` already existed.
- Verb table: `wifi scan`, `wifi station-scan`, `wifi dupe-ssid`, `sigint
  wardrive`, `tools gps` promoted reserved→LIVE; parse_command returns them as
  ("info", …) summaries (host-side, not fired blade ops).
- STILL reserved (need firmware to EMIT the frames first — nothing to analyze
  until then): wifi monitor/probe-sniff/deauth-watch/reconnect-test, all bt
  recon (needs BLE observer), nrf24 sniff/spectrum, cc1101 raw/spectrum,
  sigint eapol/pmkid/saved, tools serial/fwupdate/touch-cal/radiotest.
- VERIFIED: syntax check + self-test green — `wifi scan` lists real emitters,
  `sigint wardrive` shows GPS-tagged sightings (83 in a 2s sim run), dupe-ssid/
  station-scan/gps all return live info; eapol stays reserved, deauth/bt-beacon
  stay extern.

## Pass 9 — FH_TYPE_SSID frame + EXACT duplicate-SSID detection (2026-10-25 PM)
Joshua chose NOT to shortcut dupe-ssid with a truncated-SSID-in-extra[] hack —
full exactness instead. So a new FROZEN frame carries the full SSID:
- Protocol (frozen, all 8 copies, md5 identical, parser-verified 39 B / 0x04):
  `FH_TYPE_SSID = 0x04` + `fh_ssid_t {mac[6]; ssid_len; ssid[32]}`. Separate
  frame (NOT a capture field) so CAPTURE stays 34 B — the mapping is stable, so
  a sniffer emits it occasionally per AP; the host joins by MAC. Chosen over
  (a) enlarging fh_capture_t (breaks frozen size) and (b) 10-byte truncated SSID
  in extra[] (would false-match on shared prefixes — the shortcut we rejected).
- Medusa: client handles FH_TYPE_SSID → `_observe_ssid` fills `_ssid` (MAC→SSID),
  `on_ssid` callback added. `detect_dupe_ssid()` rewritten EXACT: flags any SSID
  advertised by ≥2 distinct BSSIDs (the evil-twin/duplicate condition), skips
  hidden (""). Parser branch renders ssid + its BSSID list.
- Simulator emits SSID frames incl. a deliberate twin ("HomeNet" on two BSSIDs)
  + a hidden SSID, so detection is exercised.
- VERIFIED: syntax check + self-test green — live `wifi dupe-ssid` correctly
  reports `"HomeNet" on 2 BSSIDs` with both addresses; self-test asserts it.
- FIRMWARE TODO: the Wi-Fi sniffer must emit FH_TYPE_SSID when a beacon/probe-
  resp carries an SSID IE (hub just relays it to Medusa like any frame).

## FIRST HARDWARE RUN (2026-10-30) — Medusa green against the real P4 hub
Ran against the real P4-WIFI6-DB on COM5 (CH343 USB-UART, 921600). End state:
`Medusa` cell green, live `BT` devices (hub-local BLE, src 5) with maker /
service / flags / TX power, 0 CRC errors. Medusa-side fixes (each marked
`BENCH FIX (2026-10-30)` in code):

| # | Symptom | Root cause | Fix (file) |
|---|---|---|---|
| 1 | Attached, but zero captures | `CStruct.unpack` demanded the full struct size; the frozen contract lets CAPTURE be SHORT (LEN = FIXED + E); the error was swallowed in `_route` | zero-pad short payloads (`medusa/protocol.py`) |
| 2 | Hub rebooted on every Medusa launch | pyserial asserts DTR/RTS on open; they drive the CH343 auto-reset (EN/BOOT) | open with DTR/RTS released (`medusa/session.py`) |
| 3 | Device table twitchy / cursor jumping | table `clear()`+rebuilt 4×/s | keyed rows, update changed cells only (`tui_textual.py`) |
| 4 | Follow-newest switched itself off | async RowHighlighted from our own `move_cursor` treated as a user move | ignore own/stale highlights (`tui_textual.py`) |
| 5 | End key dead in the table | DataTable swallowed it | `Binding("end", …, priority=True)` (`tui_textual.py`) |
| 6 | MAC `c3:81:b7:ab:ef:1a` drew a 🆎 badge | Rich markup strings expand `:ab:` emoji shortcodes | data rendered as `rich.text.Text`, never markup (`tui_textual.py`) |
| 7 | Detail pane `seen x1` forever | showed the frame's own `count` | live row counter (`tui.py`) |
| 8 | No maker / caps for BT | not decoded | `MedusaModel._ble_meta` decodes the hub's BLE `extra[10]` (`tui.py`) |

NOTE: the `link` status cell is USB *integrity* only (no CRC errors) — it is
green even with nothing plugged in. The `Medusa` cell is the real "attached and
frames flowing" light.

BLE `extra[10]` layout is owned by the hub (`fh_hub_ble.c`; also in the hub
CONTINUITY §3c): `[0]` chan · `[1]` addr type · `[2]` GAP event · `[3..4]`
company ID LE · `[5]` AD flags · `[6..7]` 16-bit service UUID LE · `[8]` TX
power · `[9]` presence bits (0x01 company, 0x02 flags, 0x04 svc, 0x08 txp,
0x10 name). The company/service name tables in `tui.py` are deliberately SHORT
(only IDs I was sure of); unknown ones print as hex. A full Bluetooth SIG table
is a possible follow-up.

## How to run
`pip install -r requirements.txt` then `python3 selftest.py` and
`python3 simdemo.py` (no hardware). TUI: `python3 tui_textual.py --sim`
(Windows-friendly) or `python3 tui.py --sim` (curses). On real hardware
(Windows, from the ESP-IDF venv which has textual + pyserial):
`python tui_textual.py COM5`. Close any `idf.py monitor` first — only one
program can hold the port. Medusa sends ATTACH (and heartbeats, which also
attach) to flip the hub into interactive mode.

## Open items
- 2026-10-30: `selftest.py`, `tui_textual.py --selftest-model` and a headless
  Textual table test all PASS; Medusa ran live against the real hub (above).
  The command verbs (ping / sample_* / nfc / lora_tx) are still unproven
  end-to-end — no blades are wired yet.
- The simulator still models generic nodes (src 1/2/3) + radio RESPs — still
  valid under the frozen protocol; a later pass could enrich it to mirror the
  exact REMIX blade set, but it is not wrong today.

## 2026-10-31 — Tier 1 + Tier 2a (written, host side BENCH-PROVEN via sim; firmware side READ-ONLY, expect a compile-fix pass)

PROTOCOL (frozen header change — all 8 copies re-vendored, md5 `94550639…`
identical: Medusa protocol/, hub components/fh_protocol, cc1101/nrf24/gps/sx1262
blades, C5/S3 sniffers):
- `FH_OD_SYS_STATE_REQ/RESP = 0x2D/0x2E` + `fh_od_sys_state_req_t` (1 B) +
  `fh_od_sys_state_resp_t` (74 B, `_Static_assert`d): uptime, bridge mode, SD
  mounted/free/used MiB, logger stats (staged/committed/rolled/dropped/res),
  requester stats (jobs/timeouts/busy), the 6 config settings + STOCK/MODIFIED,
  and last-GPS-fix lat/lon/sats. The HUB answers it itself (never a blade).

MEDUSA (all selftests + model tests pass; sim round-trips verified):
- `client.system_state()` — the SYS_STATE REQ/RESP verb.
- `client.scan_bt()` + TUI `bt scan` LIVE (client-side wiring only: src-5
  capture store filter — the hub already streams BLE; tier-1 "cheap" item).
- TUI `tools sysstate` LIVE — renders the SYS_STATE RESP human-readable.
- `client.cc1101_replay(freq_khz)` + TUI `cc1101 replay` LIVE (action=3, no
  dwell) — replays the blade's LAST captured OOK signal.
- `intents.system_state()` + `intents.subghz_replay_last()` now call the real
  atomics; `subghz_replay(dump_id)` still needs the dump store.
- TUI model tests extended (bt scan live, sysstate live, replay live-fire).

HUB FIRMWARE (written, NOT compiled — Joshua's build):
- `main/fh_hub_sysstate.c/.h` (NEW): builds the 74 B RESP from
  `fh_log_stats()` + `fh_req_stats()` + `fh_cfg_get()/fh_cfg_status()` +
  `fh_board_sd_mounted()/fh_board_sd_space()`. Bridge `on_host_frame` answers
  SYS_STATE directly (before the blade routing), like LORA_RX_IND handling.
- `fh_hub_req.c/h`: NEW `fh_req_stats()` exposes jobs_done/timeouts/busy.
  `job_duration_ms()`: CC1101 action==REPLAY now returns the floor deadline
  (a TX job must not be billed the dwell window).
- `fh_hub_board.c/h`: NEW `fh_board_sd_space()` (statvfs on the mount point).
  Include added to main/CMakeLists.txt: fh_hub_sysstate.c.
- BENCH-NOTE in code: fh_fix_t carries no sats — the RESP `sats` field is 0
  until fh_time's fix struct grows one (noted R1 in fh_hub_sysstate.c).

CC1101 BLADE (`remix-fh-pico-cc1101.ino`, written, NOT compiled):
- `service_pending()` now branches on `req.action`: REPLAY (3) replays the
  LAST captured signal via the carried `transmitCode()` (3 repeats) at
  `req.freq_khz`, ignores dwell, returns the decode triple in RESP data[]
  (proto, bits, value LE) echoing what was re-emitted; requires a prior
  capture (`g_haveCapture`) else NO_RESULT. Risk X5 updated in the file.

TIER 2 (capture-to-dump) — SHIPPED 2026-10-31 (Medusa green end-to-end on the
simulator; hub + blades written, NOT compiled — Joshua's build):
- PROTOCOL: `fh_ondemand.h` grew `FH_OD_DUMP_IND = 0x2F` — the UNSOLICITED
  chunk stream a capture-to-dump job emits BEFORE its summary RESP (the hub's
  flight completes on the RESP, so every chunk is already mirrored to Medusa).
  255 B payload: seq, radio tag (FH_DUMP_RADIO_CC1101=1 / _NRF24=2), chunk idx,
  more flag, len, data[250]. Also: `fh_od_nrf24_req_t` gained `mode`
  (SCAN=0/SNIFF=1, 5 B -> 6 B). All 8 header copies re-synced, md5 identical
  (`dfeb0d22...` — re-verify after any further edit; current after the Tier 3
  extern band: `315e4db8...`).
- MEDUSA: `client.cc1101_raw(freq, dwell)` (action=2) + `client.sniff_nrf24
  (chan, dwell)` (mode=1); `_on_dump_ind` reassembles chunk streams by seq
  (duplicate chunks dropped, real gaps recorded as `gap`), finalized dumps in
  `self._dumps` (bounded 32, newest-first via `dumps()`); OOK edges parsed as
  (dur_us, level), nRF24 payloads as (t_ms, payload). `simdemo.py` exercises
  both end-to-end.
- TUI: `cc1101 raw <freq> [dwell]` and `nrf24 sniff <chan> [dwell]` LIVE
  (fire verbs; render the dump summary + first records). Model tests extended.
- INTENTS: `subghz_capture()` and `nrf24_log()` now call the real atomics.
- HUB FIRMWARE: NO changes needed for 0x2F — `route_frame` mirrors every frame
  to Medusa (chunks flow), and `fh_req_on_frame` only tracks RESP types
  (0x21..0x2C), so the chunk stream passes through untouched. ONE FIX:
  `job_duration_ms()` re-parses the 6 B nRF24 REQ (mode-aware: SNIFF = dwell,
  SCAN = span×dwell) — the old offsets read the wrong bytes after the struct
  grew. DEADLINE MATH: dwell + the 2 s FH_REQ_MARGIN_MS covers the chunk
  stream; the 45 s FH_REQ_DEADLINE_CAP covers a 30 s blade clamp.
- CC1101 BLADE: action 2 (RAW_OOK) serializes the carried core's edge buffer
  (u32 dur_us LE + u8 level, 5 B/edge, MAX_EDGES 512) and streams it as
  DUMP_IND chunks, one per poll transaction, paced by an SPSC `s_tx_done`
  flag set in hub_on_sent(); the summary RESP (with the decode triple, so
  REPLAY-after-RAW works) is staged LAST. Risk note X7/X8/X9 added.
- nRF24 BLADE: mode 1 (SNIFF) dwells on one channel, captures every payload
  (1..32 B) as 40-byte records (u32 t_ms LE + u8 len + payload[32] + 3 pad),
  same chunk pacing + RESP-last. Risk note N7 added. Note: the blade still
  says "FRAMEWORK ONLY" elsewhere — N7 supersedes that for sniff only.
- Medusa duplicate-chunk tolerance added (a poll can clock the same chunk
  twice if a poll beats the blade's loop advance).

TIER 3 (extern verb band) — SHIPPED 2026-10-31 (Medusa green on the simulator;
hub + blades written, NOT compiled — Joshua's build). Decision: the
transmit-disruption verbs ARE authored externally (no menu options), but the
PLUMBING is wired so they work from the command line the moment the external
author's firmware is flashed. `EXTERN_CONTRACT.md` is the frozen contract.
- PROTOCOL: `fh_ondemand.h` reserves 0x30..0x3F — RESP = REQ+1, REQs EVEN,
  routed by band (0x30..0x37 = CC1101 blade, 0x38..0x3F = nRF24 blade). One
  generic REQ/RESP shape (`fh_od_ext_req_t` 246 B: seq, verb_id, p1/p2/p3 u32
  LE, tail[]; `fh_od_ext_resp_t` 253 B: seq, status, len, data[]). p3 =
  duration_ms convention = the hub's job deadline (clamped 30 s). New status
  FH_OD_EXT_STUB (0x06): routing proven, implementation not authored. Shipped
  ids: 0x30 CC-JAM, 0x32 CC-BRUTE, 0x38 NRF-JAM, 0x3A NRF-PROTOKILL; 0x34..0x36
  and 0x3C..0x3E reserved for future authors. All 8 header copies re-synced,
  md5 identical (`315e4db8...`).
- MEDUSA: `client.ext_verb()` + `_EXT_VERBS` table; TUI dispatch fires the
  blade externs from the command bar (honest STUB rendering) — wifi/bt
  disruption verbs still return "external module" (no protocol contract yet;
  hub-internal implementation would need a different contract). Model tests
  assert cc1101 jam / nrf24 protokill fire as extern STUB.
- HUB FIRMWARE: `fh_link_slot_for_radio` maps the extern band; bridge
  `is_ext_req` routes even 0x30..0x3F; `fh_req_resp_type_for` returns +1;
  `job_duration_ms` derives the deadline from p3 (bytes 10..13, 0 = floor,
  30 s cap).
- BLADES: both `on_hub_frame()` handlers gained a default-case EXTERN stub that
  decodes the generic REQ and answers FH_OD_EXT_STUB — the exact slot for the
  external author to replace.
- Drift-guard lesson: the first _Static_assert I wrote said 249 B; the real
  size is 246 B — the guard caught it before any copy was synced.

TIER 3 CONTINUATION (TX verbs authored IN-PROJECT) — SHIPPED 2026-10-31 (all
gates green on the simulator; blades written, NOT compiled — Joshua's build).
Decision revision: the tx-class verbs are NOT disruption primitives — they
transmit to the operator's OWN devices — so they were implemented in-project
(by the programming agent) rather than left to the external author. The
disruption verbs (jam/brute/protokill) REMAIN external stubs by policy.
- PROTOCOL: `fh_ondemand.h` adds 0x34/0x35 FH_OD_EXT_CC_TX_REQ/RESP and
  0x3C/0x3D FH_OD_EXT_NRF_TX_REQ/RESP (IMPLEMENTED; 0x36 + 0x3E stay reserved
  for future authors). Comment block documents both wire layouts. All 8 header
  copies re-synced, md5 identical (`36b38a1d...` — re-verify after any edit).
- CC1101 TX (0x34): p1=freq_khz, p2=bits 1..32, p3=0; tail[0]=proto,
  tail[1..4]=value u32 LE, tail[5]=repeats (0→3). Blade runs the carried
  `transmitCode()` core (same one REPLAY uses; re-enters RX after, so capture
  keeps working). RESP: len=6 echo (proto, bits, value LE). BAD_REQ on
  !g_radioOk / bits out of range / proto 0 or ≥ NUM_RC_PROTOCOLS.
- nRF24 TX (0x3C): p1=chan 0..125, p2=repeats (0→1, cap 16), p3=0;
  tail[0]=payload_len 1..32, tail[1..]=payload; frame length ≥ 15+payload_len
  (the ext header is 14 B). Blade writes on the RF24 library DEFAULT address
  (0xE7E7E7E7E7 — the same address its sniffer listens on, so TX→RX is provable
  on the bench), autoAck off, restores idle (stopListening + home channel)
  after. RESP: len = 1+plen+1, data = len byte + payload echo + acked-writes.
- MEDUSA: `client.cc1101_tx(freq, proto, bits, value, repeats)` +
  `client.nrf24_tx(chan, payload, repeats)` (validate client-side; the tail
  shapes above). TUI: `cc1101 tx <freq> <proto> <bits> <value> [repeats]` and
  `nrf24 tx <chan> <payload> [repeats]` LIVE (payload accepts 0x-hex, 0b-binary
  or raw text). `_od_status` now renders enum names ("FH_OD_OK") instead of raw
  ints. Model tests: both verbs round-trip through the simulator with the echo;
  bad bits rejected.
- SIMULATOR: `FH_OD_EXT_CC_TX_REQ`/`FH_OD_EXT_NRF_TX_REQ` emulated (same echo
  and BAD_REQ behavior as the blades); other ext verbs still answer STUB.
  `simdemo.py` fires both TX verbs as demos.
- HUB: NO changes — routing (0x30..0x37→CC1101, 0x38..0x3F→nRF24), RESP=REQ+1,
  and the p3=0→floor-deadline rule already covered the TX verbs.

TIER 3 ADDENDUM (replay-from-store) — SHIPPED 2026-10-31 (all gates green).
`cc1101 replay <freq> <seq>` re-transmits a SPECIFIC captured signal from
Medusa's own decode-triple store (NOT the blade's last capture — the blade may
have re-captured or power-cycled since). Pure host-side composition: no new
protocol, no blade change — it fires the implemented 0x34 ext-band TX verb
with the stored triple.
- MEDUSA: `client._cc_captures` (seq -> {seq, freq_khz, proto, bits, value, t},
  bounded 32) filled by `_remember_capture()` on every successful CC1101
  capture RESP (LISTEN and RAW_OOK both echo the triple); `cc1101_replay_from
  (seq)` fires the 0x34 verb and tags label "CC1101 REPLAY-FROM" +
  source_seq; `cc1101_captures()` lists the store newest-first.
- TUI: `cc1101 captures` (live, lists the store) and `cc1101 replay <freq>
  <seq>` (live, replay-from detail). Model tests: store→captures→replay-from
  round-trip through the simulator; unknown seq = honest KeyError.
- SIMULATOR: capture RESP aligned to the blade's actual shape (len=6 triple,
  was 8 B) for LISTEN and RAW_OOK summary; `simdemo.py` demo: raw capture →
  store → replay-from echo matches.
- HEADER: fixed the STALE fh_ondemand.h comment that called REPLAY "reserved /
  saved SD file" — the blade has implemented it as last-capture replay since
  the Tier 1 pass. All 8 copies re-synced, md5 `6776074475` (re-verify after
  any further edit).
- NOTE: replaying a RAW OOK dump (arbitrary edges, not a decode triple) is
  still a blade-side feature (pulse-train playback) — NOT this verb; it stays
  proposed.

NEXT: bench-verify on real hardware (raw capture -> dump in Medusa; sniff with
a known transmitter; cc1101 jam -> STUB through the real hub/blade). Then the
Tier 3 conversation is DONE: the extern verbs are wired and wait only on the
external author's firmware.
