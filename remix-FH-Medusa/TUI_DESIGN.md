# FH_Medusa — TUI DESIGN (whiteboard, v2 — the three-stream model)

> STATUS: design notes (2026-10-22). Not built yet — tui.py is an earlier basic
> version. This v2 supersedes the earlier two-column sketch. Same design also
> drives the field GUI (the `m` menu tree is that device's whole interface).

## The core idea: SEPARATE "watch" from "touch"

The screen has THREE event surfaces, each with ONE job. The key insight is that
observation and interaction are different activities and should not share a pane —
that is what dissolves all the cursor-yank / sort / focus-stealing problems.

```
┌─ STATUS BAR (always, expanded) ───────────────────────────────────────────────┐
│  Medusa │ Display  X-S3  X-C5 │ GPS●8 40.742,-73.981 │ link                    │
├─ LIVE STREAM (untouched, never-ending, READ-ONLY) ─────────────────────────────┤
│  CAP x-c5(5) a4:… rssi=-52    CAP BT 5c:… -44 AirPods    GPS sats=9             │
│  CAP x-s3(2.4) d0:… rssi=-61    SUM pico count=14    CAP BT e0:… -71            │
├─ INTERACTION COLUMN ───────────────────┬─ DETAIL — "the full monty" ───────────┤
│  (the ONLY place you act; MODAL —       │  everything about whatever is          │
│   shows ONE of three things:)           │  selected on the left:                 │
│   • dedup'd DEVICE LIST (navigable)     │  full decode · stats · history ·       │
│   • the m MENU tree                    │  shape strip · complete record         │
│   • fired-COMMAND RESULTS you're        │                                        │
│     interacting with                    │  (always reflects the left selection)  │
│   fills to the NOW-line                 │  fills to the NOW-line                 │
├──────────────────────────────────────────┴─────────────────────────────────────┤
│ NOW: ▸ streaming…          OR          ▸ CC1101 433.92 recording  3.2 / 5.0s    │
└──────────────────────────────────────────────────────────────────────────────────┘
      (the c command bar pops HERE, over the NOW-line, when summoned)
```

## The four zones

1. **STATUS BAR** — always, expanded. Per-node health lights + GPS fix + link
   integrity, all color-coded. Never collapses. Pure glance value. REDESIGNED
   for REMIX — see "## STATUS BAR (REMIX redesign)" below for the authoritative
   element list, colors, and layout.

2. **LIVE STREAM** — top, raw, never-ending, **READ-ONLY**. Time-ordered flow of
   everything (captures/GPS/BLE/HB). You WATCH it; you never touch it. It is the
   system's pulse. Because it is never navigable, it can never yank a cursor or
   steal focus — that is the whole point of keeping it separate.
   SOURCE NAMES (one map, `MedusaModel.src_name`): 1=`x-c5(5)` 2=`x-s3(2.4)`
   5=`BT` (hub-local BLE), fallback `src{N}`. The head (4) is NOT a stream source
   (render sink). NO `pico` (3): the Pico only heartbeats (not streamed).
   TWO KINDS of stream line:
     • PASSIVE captures (Wi-Fi/BLE) — continuous, per-detection:
       `CAP x-s3(2.4) a4:… rssi=-52`.
     • FIRED blade ops (CC1101/nRF24/NFC/…) — a COMMANDED action, named by the
       RADIO (never "pico"), with a per-ACTION summary: a DURATION for a timed
       capture (`CAP CC1101 — 15 SEC`) or an OUTCOME for a one-shot function
       (`CAP CC1101 — REPLAYED`, `CAP NFC — MIFARE 1K`). Emitted by the shared
       `MedusaModel.fire_result(radio, summary, detail)` — one source of truth,
       both TUIs call it; it pushes the stream line AND the ★ detail row.
   Verbs shortened for width: `CAP` (capture), `SUM` (summary), `GPS`. Height is
   proportional (~1/3, floor 5 lines) in both renderers. NO per-line timestamp:
   the stream IS "now" (newest = current, order = sequence); exact time lives in
   the SD log (hub-written) + a selected event's detail, not every line.
   COLOR (black bg; no filtering — nothing is ever muted): each line is colored
   by SOURCE at creation — BT=blue, x-s3(2.4)=green_light (bright), x-c5(5)=
   green_dark (plain green), GPS=yellow, SUM=white, fired radio ops=orange.
   Stored as (text, color) tuples (one source of truth); curses maps to color
   pairs (orange=256-color 208, else yellow+bold), Textual via RichLog markup.

3. **INTERACTION COLUMN (left)** — the ONLY place actions happen. MODAL: it shows
   exactly one of three things at a time —
   - **Device list** — dedup'd, one row per identity (MAC / node / commanded ★),
     "seen 3×", updates IN PLACE (new RSSI/MCS/channel/last-seen refresh the row);
     a new distinct device adds a row. Navigable with ↑↓. SELECTION is
     IDENTITY-STICKY, not a raw index: the highlight is glued to the selected
     row's KEY, so rows appending below never slide the cursor off your item
     (the old index-based bug: highlight drifted up as the list grew, and
     reaching the newest row by arrowing was unwinnable). FOLLOW-NEWEST is ON by
     default (cursor rides the latest arrival); pressing ↑/↓ disengages it so you
     can inspect in peace while the stream flows; `End` (curses also `G`;
     Textual `end`) re-arms follow and snaps to newest. Firing a command pins
     the highlight to its ★ result (focus follows intent, follow off). The NOW
     line shows `▶following` when follow is active.
   - **m menu** — the discoverable Flipper-style intent tree (single press of
     `m`; alt combos don't decode reliably). Pops up OVER the left pane (modal),
     preview-on-highlight in DETAIL, enter-to-run, no immediate-fire, ⚠ =
     active/confirm. This is how single fire-once ops NOT typed on the command
     line are chosen.
   - **Command results** — fired commands you are actively working with.

4. **DETAIL (right) — "the full monty"** — the complete record of whatever is
   selected on the left. Always mirrors the left selection. Per-type RECORDS
   (clean labeled metadata — we observe announcements/presence, NOT payload; no
   raw struct dumps):
   - **Wi-Fi**: Wi-Fi + node · time · mac · frame (beacon/probe-req/… decoded
     from frame_type) · band + channel · rssi · seen-count.
   - **BT (BLE)**: time · addr · rssi · announced caps · manufacturer (decoded
     from the company-ID, via the hub's BLE `extra[10]`; live since 2026-10-30
     — see CONTINUITY.md "FIRST HARDWARE RUN").
   - **GPS**: time · lat · lon · sats · fix (valid / acquiring).
   - **Fired radio op**: echoes the EXACT command string typed + confirms the
     full duration was recorded (`recorded 15s`) + a one-line readable result
     (status/rssi/etc. via `_od_status`) — NOT the wire struct.

5. **NOW-line (bottom)** — one live status line for YOUR current action:
   `streaming…` when nothing is commanded (just watching), OR a countdown when a
   radio op is running (`CC1101 433.92 recording 3.2/5.0s`). The `c` command bar
   appears here when summoned; on enter the command fires, shows status here, and
   its RESULT returns as an event (auto-focused in DETAIL — "focus follows intent").

## Locked behaviors (from the Q&A)
- **Dedup, update-in-place** — same MAC updates its one row (seen-count ticks,
  stats refresh); it does NOT add a new line. (#Q2/#Q-dedup resolved.)
- **Live stream is never interacted with** — so there is no sort/yank problem up
  top; the left device list is where you navigate. (#Q3 resolved by the watch/
  touch split — the thing you navigate is distinct from the thing that floods.)
- **Command lifecycle** (#Q4 resolved): enter in the `c` command bar → fires (unless immediate
  error) → status in the NOW-line → on return it appears as an event in the left
  + auto-displays in DETAIL. No separate "pinning"; it's an event like any other
  that happens to auto-focus because you asked for it.
- **Command GRAMMAR**: `<module> <verb> <args>`, human units (`11sec`/`500ms`/
  `2s`/bare=ms). One shared parser (`MedusaModel.parse_command`/`run_command`),
  both TUIs. Three verb states: `live` (wired now), `reserved` (observational/
  capture, recognized, pending firmware), `extern` (verb recognized but its
  transmit-disruption primitive is authored by an EXTERNAL module, not here).
  BUILD POLICY: capture/recon/detect = built; transmit-to-disrupt primitive =
  verb present, authored externally. Full HALEHOUND verb surface:
    cc1101: `cap`(live) · `replay`(live: replays LAST capture; `replay <freq> <seq>` replays a SPECIFIC capture from Medusa's store via the implemented 0x34 TX verb) · `captures`(live: lists the decode-triple store) · `raw`(live: capture-to-dump) · `tx`(live: raw OOK transmit to YOUR OWN devices — ext band 0x34 IMPLEMENTED in the blades; EXTERN_CONTRACT.md §7) · `spectrum`(reserved) · `jam`,`brute`(extern: WIRED — command-line only, blade answers STUB until the external author's firmware; EXTERN_CONTRACT.md)
    nrf24:  `scan`(live) · `sniff`(live: capture-to-dump) · `tx`(live: payload transmit to YOUR OWN receiver — ext band 0x3C IMPLEMENTED in the blades; EXTERN_CONTRACT.md §7) · `spectrum`(reserved) · `jam`,`protokill`(extern: WIRED — command-line only, blade answers STUB until the external author's firmware; EXTERN_CONTRACT.md)
    nfc:    `read`,`emulate`,`write`(reserved — NFC PARKED 2026-11-01, see ../remix-PARKED-CARD-READER-RADIOS.md)
    lora:   `tx`,`listen`(live)
    wifi:   `scan`,`station-scan`,`dupe-ssid`(LIVE, host-side analytics) ·
            `monitor`,`probe-sniff`,`deauth-watch`,`reconnect-test`(reserved) ·
            `deauth`,`beacon-spam`,`auth-flood`,`evil-twin`,`captive-portal`(extern)
    bt:     `sniff`,`scan`,`airtag-detect`,`stalkerware-detect`,
            `whisperpair-scan`(reserved) · `beacon`,`jam`,`spoof`,`pair-spam`(extern)
    sigint: `wardrive`(LIVE, capture↔GPS) · `eapol`,`pmkid`,`saved`(reserved —
            EAPOL/PMKID are CAPTURE; cracking is off-device/out of scope) ·
            `karma`(extern)
    tools:  `gps`(LIVE) · `serial`,`fwupdate`,`touch-cal`,`radiotest`(reserved)
    ping (no verb, live)
  Mechanism: CC1101 live/reserved verbs ride the protocol's `action` sub-field,
  named `FH_OD_CC1101_ACT_{LISTEN,RX_ONE,RAW_OOK,REPLAY}` in fh_ondemand.h
  (additive, all 8 copies). `extern` verbs return a clear "external module"
  message — recognized, no crash, no attack scaffolding. `wifi/bt/sigint/tools`
  are command-bar MODULES, not new wire src_ids.
- **Overlap is intentional** — the same events appear both in the top live stream
  (to watch) and, dedup'd, in the left column (to touch). Two views, two jobs.

## Still open (circle back)
- **#Q1 what a row is** for non-MAC things — the device list is mixed (MAC devices
  + node health + commanded ★). Grouped/sectioned vs flat: UNDECIDED.
- **left-column mode switching** — how it flips between device-list / menu /
  command-results (keys, and what's default).
- **#Q5 confirm modal** for ⚠ active verbs (how yes/no, fat-finger resistance).
- **#Q6 the 1-D shape strip** rendering (how `▁▁█▁▁█` draws) — downstream, only
  matters once we render captured RF.

## This is also the field GUI
The `m` menu tree = the field handheld's whole interface (d-pad, no command
bar, no keyboard). The TUI adds the live stream, the command bar, and the richer
detail. Same menu, two renderers; the field one is the spartan subset.

---

## STATUS BAR (REMIX redesign) — 2026-10-25

Authoritative spec for the top status bar, decided piece-by-piece with Joshua.
Supersedes the gen1 bar (`●hub ●C5 ●S3 ○Pico  GPS✓ …  f1284 crc0  17:36`). This
is a MEDUSA-side redesign; where an element needs hub/firmware support it is
noted. The wire protocol is unchanged except the additive `FH_SRC_HEAD=4` enum
value (already applied + md5-verified across all 8 copies).

### Layout (left → right: the connection, then what's on it, then integrity)

```
 Medusa │ Display  X-S3  X-C5 │ GPS●8 lat,lon │ link
```

- Order principle: ROOT first (Medusa), then the things it's wired to, then "is
  the data clean" last.
- `│` groups: {Medusa} | {the 3 Bus-A peers} | {GPS, a Bus-B blade} | {link}.
- Each indicator is a **word that carries color**, not a bare colored dot
  (unambiguous in a terminal, not color-only). The `●` in `GPS●8` only anchors
  the sat count; the word still carries primary color.
- **Degraded states add text, never reflow.** Slots keep fixed positions; a bad
  state changes color and may append a suffix (`link ⚠ 3 crc/s`), but nothing
  jumps sideways. A shifting bar is unreadable at a glance.
- Exact column widths/padding are per-renderer (curses `addnstr` fixed-width vs
  Textual CSS) — set at build time, not fixed here.

### Elements (the six banked decisions)

1. **`Medusa`** — the app title doubles as the HUB CONNECTION light.
   - GREEN = attached to the P4 **AND** frames actively flowing (a good frame
     within ~4 s — ≈2–3× the ~1.5 s heartbeat cadence).
   - RED = not attached, or attached but the link has gone silent.
   - Absorbs the old `[ATTACHED]/[no-ack]` field (no longer separate).
   - Owns LIVENESS. ("are we connected and getting data")

2. **`Display`** — Waveshare-S3 head (Bus-A peer).
   - GREEN = the hub has an ACTIVELY-CONFIRMED BIDIRECTIONAL link with the head
     (hub PING → head PONG, and/or head HEARTBEAT) within the freshness window.
     Medusa reads this from the hub's health view (node 4), NOT by seeing head
     frames directly — the head is a render sink, not a data source.
   - RED otherwise.
   - DEPENDENCY: hub side is wired (health node 4 + PING routing, see hub
     CONTINUITY §3b). Shows RED honestly until HEAD FIRMWARE exists to answer the
     PING. Flips green with zero further hub change once it does. We deliberately
     did NOT fake green off the one-way display push.

3. **`X-S3 (2.4)`** — XIAO-S3 2.4 GHz Wi-Fi (Bus-A peer, src_id 2).
   - GREEN = Medusa sees recent capture frames carrying src_id 2 (within window).
   - RED otherwise.

4. **`X-C5 (5)`** — XIAO-C5 5 GHz Wi-Fi (Bus-A peer, src_id 1).
   - GREEN = recent capture frames carrying src_id 1.
   - RED otherwise.

5. **GPS** — GPS blade (Bus-B). First-class because it's on essentially every
   FootHoled device. THREE-TIER (two colors would lie on cold-start):
   - GREEN = recent `GPS_FIX` frame AND `fix_qual > 0` (valid fix; show sats,
     e.g. `GPS●8`). Position/time trustworthy.
   - AMBER = recent `GPS_FIX` frame but `fix_qual == 0` (blade alive, acquiring —
     cold start / indoors). NORMAL, not an error — amber exists so power-on
     doesn't cry wolf in red for minutes.
   - RED = no `GPS_FIX` frame within the poll window (blade silent/dead/absent).
     Window aligns to `cfg.gps_rate` (1 or 5 Hz) so a slow poll doesn't false-red.
   - Fields confirmed present in frozen `fh_gps_fix_t`: `fix_qual`, `sats`.
   - BENCH-VERIFY: the AMBER tier assumes the GPS blade EMITS a `GPS_FIX` frame
     with `fix_qual=0` while unfixed (not silence-until-lock). If the blade only
     sends on lock, amber collapses into red and the blade firmware needs a small
     touch to emit no-fix frames. Medusa display logic is correct either way.

6. **`link`** — Medusa↔hub USB link INTEGRITY (replaces raw `good:N crc:N`).
   Owns INTEGRITY only (the title owns liveness — no overlap).
   - GREEN / quiet = clean: no CRC errors in the recent window (~2–3 s). Stays
     green even if the stream goes SILENT (silence isn't corruption).
   - LOUD (amber/red) + rate = CRC errors RISING in the recent window, e.g.
     `link ⚠ 3 crc/s`. The number appears only when it matters.
   - "Rising" = crc DELTA over a recent window, not the lifetime total — so a
     startup-resync artifact fades back to green instead of nagging forever.
   - IMPL NOTE: codec exposes `good_frames`/`crc_errors` as lifetime cumulative;
     Medusa's ~4 Hz refresh samples + diffs against the previous sample to get the
     rate. Pure Medusa change — no protocol/hub change.

### Removed
- **`HH:MM:SS` wall clock** — host clock, redundant with the laptop's own clock;
  reclaimed the space. (Removed from BOTH tui_textual.py and tui.py status
  strings; leave `import time` — other uses, e.g. stream timestamps — intact.)
- **`nodes:N`** raw count — replaced by the per-peer lights above (it under-
  counted: heartbeat-senders only, no head/GPS, collapsed the REMIX blade split).

### Note / open door
GPS is the first indicator with an AMBER tier. The Bus-A peers could later gain
an "alive but not capturing" amber — the hub health model already computes this
as `QUIET_BUT_ALIVE`. NOT banked; flagged for a future pass.
