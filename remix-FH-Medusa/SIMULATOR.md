# FH_Medusa — SIMULATOR (run it with no hardware)

`medusa/simulator.py` is a FAKE HUB. It produces a realistic `fh_protocol`
stream (captures from a small device population so dedup shows "seen N×", GPS
fixes ~1 Hz, heartbeats, ...) and answers on-demand commands (ping / cc1101 /
nrf24 / nfc / lora) with plausible results — honoring the one-at-a-time BUSY
timing (a dwell takes real wall-clock time, like a real radio blade which responds
only AFTER the listen completes).

It presents the same `read()/write()/close()` surface a serial port does, so it
drops into the client in place of real hardware. It is DRIFT-PROOF — it builds
every frame from the same parsed headers as the rest of Medusa.

## Two ways to run it

### 1. Text demo — runs in ANY terminal, no curses, no deps
```
cd FH-Advanced-Medusa-EXPERIMENTAL
python3 simdemo.py
```
Prints ~3 s of the live event stream, then fires ping / nrf24 scan / NFC read
(the sim presents a MIFARE Classic 1K, so you'll see it classify as
`MIFARE_CLASSIC_1K`) and a deliberate BUSY collision (two radio ops at once →
the second is refused with `Busy`, proving one-at-a-time). Good first look.

### 2. Full interactive TUI — needs a real terminal (not this chat / not a pipe)
```
cd FH-Advanced-Medusa-EXPERIMENTAL
python3 tui.py --sim
```
The curses UI comes up driven by the simulator: live stream, the dedup device
list, detail pane, command line. Drive it with the keyboard. (`q` to quit.)
This is the thing to play with to feel the TUI_DESIGN out. It needs an actual
interactive terminal to render — it will not display inside a pipe or a chat
window.

## Requirements
- Python 3 (standard library only). `pyserial` is NOT needed for the simulator —
  only for talking to a REAL device later.
- No hardware. No network. Everything is local and in-process.

## What it proves (and doesn't)
- PROVES: the HOST side end-to-end — protocol parse, framing/CRC, the event
  fan-out, the request/response + BUSY lock, the client verbs, the intents, and
  (with tui.py) the UI. All without a device.
- Does NOT prove: the FIRMWARE. When hardware exists, point the client at the
  real serial port instead of the simulator (`MedusaClient(port).connect()` with
  no `use_sim`), and the same code drives the real hub.

## Wiring (how --sim attaches)
`MedusaClient.connect(use_sim=True)` swaps a `SimHub` in where the serial port
would go. Everything above that line (session/client/intents/TUI) is identical
whether it's talking to the simulator or a real hub — that's the point.

## TUI keys (tui.py --sim)
- `↑ / ↓` — navigate the left device/event list (selection shows on the right)
- `c` — open the command bar (type e.g. `ping`, `nrf24 0 80`, `cc1101 433920 2000`, `nfc`; Enter runs, esc cancels)
- `m` — open the menu overlay (↑↓ to browse radios, esc closes)
- `q` — quit
- NOTE: plain `c`/`m` are the reliable triggers. True alt+C/alt+M also work in
  terminals that pass them through, but alt-key decoding is terminal-dependent,
  so `c`/`m` are the documented path.
- Headless check (no terminal needed): `python3 tui.py --selftest-model`

## Two TUIs — pick by platform
- **tui_textual.py** — RECOMMENDED, cross-platform incl. Windows/PowerShell.
  `pip install textual` then `python3 tui_textual.py --sim`. alt+C/alt+M and c/m
  all work. Status bar · live-stream log · device table (★ commanded rows) ·
  detail · command bar. (The `m` menu is currently a stub hint — the `c` command
  bar fires every verb; a full menu screen is a follow-up.)
- **tui.py** — stdlib curses fallback, zero deps. Great on macOS/Linux/WSL, rough
  on native Windows. `python3 tui.py --sim`. Keys: ↑↓ · c · m · q.
- Both share ONE MedusaModel (UI-agnostic state). Headless check either:
  `python3 tui.py --selftest-model`  /  `python3 tui_textual.py --selftest-model`.
