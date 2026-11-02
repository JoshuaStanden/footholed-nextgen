# FH_Medusa — experimental bench-face for FH-Advanced

> ⚠️ **EXPERIMENTAL. Self-contained. Touches nothing else.** This is a host-side
> (laptop) Python program — the "face" of the FootHoled-REMIX system over the
> ESP32-P4 hub's USB-serial console. It is NOT firmware, NOT part of the build,
> and it modifies none of the firmware zips. It only *consumes* the frozen
> `fh_protocol` those zips already define. Delete this whole folder and the
> device is unaffected.

## ★ START HERE (future Joshua / future Kiro)
You can understand this whole package from the files alone — you do not need the
chat it came from. Read in this order:
1. **README.md** (this file) — what it is, how to run it, the API surface.
2. **SIMULATOR.md** — run it right now with no hardware. FIRST: `pip install
   textual` (or `pip install -r requirements.txt`). Then `python3 tui_textual.py
   --sim`. (Text-only, no deps: `python3 simdemo.py`.)
3. **IMPLEMENTATION.md** — what is PROVEN vs ASSUMED, the risks, how to extend.
4. **API_DESIGN.md** — the two-layer API map (Layer 1 brain / Layer 2 intents).
5. **TUI_DESIGN.md** — the bench-UI design (also the field-GUI spec).
6. **ROADMAP.md** — every decision we made + what's deferred + hardware gaps.

### File map
```
README.md IMPLEMENTATION.md API_DESIGN.md TUI_DESIGN.md SIMULATOR.md ROADMAP.md
requirements.txt  — pip install -r requirements.txt   (installs textual)
selftest.py   — prove the stack, no hardware        (python3 selftest.py)
simdemo.py    — watch it run against the simulator  (python3 simdemo.py)
tui.py          — curses TUI (stdlib fallback)       (python3 tui.py --sim)
tui_textual.py  — Textual TUI (RECOMMENDED; Windows)  (pip install textual; python3 tui_textual.py --sim)
medusa/
  protocol.py  — DRIFT-PROOF layouts parsed from the vendored headers
  codec.py     — framing + CRC-16/CCITT-FALSE
  session.py   — serial/sim transport, event vs response fan-out, BUSY lock
  client.py    — MedusaClient: the verbs + events (connect(use_sim=True) for sim)
  intents.py   — Layer-2 composed intents (badge.identify/copy, subghz.*, etc.)
  simulator.py — the fake hub (no hardware needed)
protocol/fh_frame.h  fh_ondemand.h  — vendored copies of the FROZEN firmware headers
```

### Decisions already settled (don't re-litigate — see the cited doc)
- **Hub is the ARBITER; Medusa talks to the hub, not the Pico directly.** (ROADMAP)
- **Gen2: the ESP32-P4 head inherits the hub's arbiter role.** (ROADMAP)
- **No on-device AI** — device = sensor+logger+status; intelligence lives in Medusa. (ROADMAP)
- **Layer 1 is complete + NEUTRAL (transmit included); we ship only legitimate
  Layer-2 intents; no "bad" functions.** (intents.py header, API_DESIGN)
- **LoRa = comms (hub↔sensor-stick); transmit is CORE. LoRa-capture is "not built
  yet," explicitly not ruled out.** (intents.py, ROADMAP)
- **NFC scoped to 13.56 MHz (PN532); identify-then-branch (Flipper-style).
  125 kHz LF needs a 5th radio — documented hardware gap.** (API_DESIGN, ROADMAP)
  **⏸ PARKED 2026-11-01:** all card-reader radios (PN532 NFC + LF/HF RFID) are
  deferred — dedicated read/write/copy/emulate tools already exist. The design
  stays as the plan of record if NFC returns; `nfc read` is reserved in the TUI.
  See `../remix-PARKED-CARD-READER-RADIOS.md`.
- **TUI = three-stream model (watch vs touch); focus-follows-intent; dedup
  update-in-place.** (TUI_DESIGN)
- **Drift is structurally impossible** — Python layouts are parsed from the C
  headers + checked against their `_Static_assert`. (protocol.py, README below)

## What it is (and why)
The REMIX hub (**ESP32-P4-WIFI6-DB**) is headless by design — the data-collection
spine runs over the hub's USB-C. The hub coordinates its peripherals over two
SPI buses (**Bus A** quad-SPI to the XIAO-C5 5 GHz + XIAO-S3 2.4 GHz Wi-Fi peers
and the Waveshare-S3 display head; **Bus B** conventional SPI to the Pico+radio
blades — CC1101 / nRF24 / PN532 / SX1262 / GPS), plus hub-local BLE on the P4's
onboard C6. Every node's `fh_protocol` frames flow into the hub and out the one
USB-C cable. **Medusa is the head that lives on your laptop:** it decodes that
stream live AND sends on-demand radio commands back (GPS poll, "sample 15 s of
nRF24", NFC read, LoRa TX).

**What the REMIX re-architecture did NOT change for Medusa:** the Medusa↔hub
link is still **USB** (the hub's USB-C = a CH343 USB-UART, 921600; run
`python tui_textual.py COM5`), and the wire
contract is still the **byte-identical `fh_protocol`** (the vendored headers in
`protocol/` match the firmware's frozen ones). So Medusa works against the P4
hub unchanged — only the hub's INTERNAL transport to its peers/blades moved from
UART to SPI, which Medusa never saw. (The gen1 version of this doc described the
Waveshare-S3-as-hub + 3 UART links; that was the pre-REMIX topology.)

## Status: proven HALF
- The **host side is proven by running** — `python3 selftest.py` exercises the
  whole stack with no hardware (drift guard, CRC known-answer 0x29B1, event
  decode, REQ/RESP, BUSY lock). All pass.
- The **firmware side is NOT yet proven** — the nodes must actually speak these
  frames on real hardware. Medusa proving its half means: when something doesn't
  work on the bench, Medusa is unlikely to be the cause.

## Architecture (4 layers, each only talks to the one below)
```
  tui.py                 you type / watch         (a client; zero protocol knowledge)
  medusa/client.py       MedusaClient API         ping/gps/sample_*/read_nfc/lora_tx + on_* events
  medusa/session.py      serial + fan-out + BUSY   reader thread; req/resp vs events; one-at-a-time lock
  medusa/codec.py        framing + CRC             encode + resync-safe streaming decode
  medusa/protocol.py     DRIFT-PROOF layouts       parses the vendored C headers -> python struct fmts
  protocol/*.h           vendored frozen headers   byte-identical copies of the firmware's fh_frame.h / fh_ondemand.h
```

## The drift-proof design (read this — it's the important bit)
Medusa must agree with the firmware on the exact bytes on the wire. Instead of
hand-copying struct offsets into Python (which rots the moment the C changes),
`medusa/protocol.py` **parses the vendored `protocol/*.h` headers at import** and
derives every struct layout from them. It then checks each parsed struct size
against that header's own `_Static_assert(... == N)` — if they disagree it raises
`DriftError` and refuses to run. So drift is structurally impossible: change a
struct in the firmware, re-copy the two headers into `protocol/`, and Medusa
re-derives (or fails loudly). **Never hand-edit a layout in Python.**

Keeping the vendored headers fresh: they are plain copies of
`components/fh_protocol/include/fh_frame.h` and `fh_ondemand.h` from any
FH-Advanced firmware zip (all four carry byte-identical copies). If the frozen
protocol ever changes, re-copy both files into `protocol/` and re-run `selftest.py`.

## Run it
```
pip install -r requirements.txt     # installs textual (the recommended TUI)
#   ^ or just:  pip install textual

python3 selftest.py                 # no hardware — proves the stack (no deps)
python3 tui_textual.py --sim        # the TUI, driven by the simulator (no hardware)
```
`tui_textual.py` is the recommended, cross-platform TUI — **it needs `textual`**
(`pip install textual`). Works in Windows Terminal / PowerShell / VS Code / macOS
/ Linux. Without textual it prints the install hint; a stdlib curses fallback is
`python3 tui.py --sim` (no install, but rough on native Windows).

Talk to REAL hardware instead of the simulator:
```
pip install pyserial                             # only for a real device
python3 tui_textual.py COM5                       # Windows
python3 tui_textual.py /dev/tty.usbmodemXXXX      # macOS  (/dev/ttyACM0 Linux)
```
Find the port: macOS `ls /dev/tty.usb*`, Linux `ls /dev/ttyACM*`, Windows Device
Manager. Baud defaults to 921600 (see IMPLEMENTATION.md).

## API surface (what the TUI, or your own scripts, call)
```python
from medusa import MedusaClient
m = MedusaClient("/dev/tty.usbmodemXXXX"); m.connect()

m.on_gps(lambda fix: ...)        # unsolicited GPS fixes (~1 Hz)
m.on_capture(lambda cap: ...)    # 5 GHz / 2.4 GHz capture rows
m.on_heartbeat(lambda hb: ...)   # per-node liveness
m.on_lora_rx(lambda rx: ...)     # inbound sensor-stick LoRa

m.ping()                         # Readiness(status, radios_ready={CC1101:bool,...})
m.gps()                          # latest fix seen (also streams via on_gps)
m.sample_nrf24(lo, hi, dwell_ms) # scan a 2.4 GHz channel range
m.sample_cc1101(freq_khz, dwell_ms)
m.read_nfc(timeout_ms)
m.lora_tx(data: bytes)
```
All on-demand verbs are **serialized** by the one-at-a-time BUSY contract: a
second request while one is running raises `medusa.session.Busy`. This mirrors
the Pico firmware's rule (it services one radio op at a time).

## See also
- `IMPLEMENTATION.md` — for future-Kiro: how this was built, what's proven vs
  assumed, the open items, and exactly how to extend it.
- `SIMULATOR.md` — run Medusa with NO hardware (simdemo.py text demo; tui.py --sim interactive).
- `TUI_DESIGN.md` — the whiteboard TUI design (layout, c/m command & menu overlays, passive-vs-commanded rows, focus-follows-intent) + open questions.
- `API_DESIGN.md` — forward-looking two-layer API map (Layer 1 brain capabilities, Layer 2 Medusa intents; PROPOSED, not built).
- `ROADMAP.md` — decisions + deferred ideas from the design conversation (hub-as-arbiter, no on-device AI, device=capture/log, viz is lossy, etc.).
- The frozen contract lives in `protocol/fh_frame.h` + `protocol/fh_ondemand.h`.
