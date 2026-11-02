# remix-FH-Medusa — WIRING

> Medusa is a **host-side laptop program** — it has no wiring of its own. This
> file exists only to complete the five-doc scheme and to record the ONE physical
> fact that matters: how Medusa connects to the hub.
>
> Five-doc set for this package: **WIRING** (this file) · **README** (people) ·
> **AGENTS** (AI/LLM) · **CONTINUITY** (resume) · **FUTURE** (planning). Medusa
> also carries richer design docs — see the README "START HERE" reading order
> (API_DESIGN, TUI_DESIGN, SIMULATOR, EXTERN_CONTRACT, IMPLEMENTATION).

## The one connection
Medusa talks to the **ESP32-P4 hub over the hub's single USB-C cable** — which is
an onboard **CH343 USB-UART on UART0**, not USB-Serial-JTAG. There is no GPIO
wiring, no soldering, no second board.

- **Baud: 921600** (fixed; the hub's console is set to this).
- **Line endings: LF-only**, both directions, so the binary `fh_protocol` frame
  stream is byte-transparent.
- **One program at a time** can hold the port: close `idf.py monitor` before
  starting Medusa, and quit the monitor with ONE Ctrl+].
- Medusa sends `ATTACH` to flip the hub from MONITOR (human logs) into
  INTERACTIVE (binary frames). It opens the port with DTR/RTS **off** so the
  CH343's auto-reset lines don't reboot the hub on connect.

## Running against hardware vs the simulator
- No hardware: `python3 tui_textual.py --sim` (or `python3 simdemo.py`).
- Real hub: `pip install pyserial`, then
  `python3 tui_textual.py COM5` (Windows) /
  `python3 tui_textual.py /dev/tty.usbmodemXXXX` (macOS) /
  `/dev/ttyACM0` (Linux).

The hub-side wiring (the two SPI buses, the microSD, the onboard C6 BLE) is in
`../remix-FH-P4-hub-firmware/WIRING.md` — that is where the actual pins live.
Medusa only ever sees the one USB cable.
