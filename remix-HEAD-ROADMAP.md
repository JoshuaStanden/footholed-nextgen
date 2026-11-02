# remix-HEAD-ROADMAP.md — the face's planned evolution (8.8" DSI on a 2nd ESP32-P4)

**Status: PLANNED, not started.** Direction of travel for the FootHoled HEAD (the
face). The current head firmware (`remix-FH-WS-head-firmware/`) stays the working
head until this lands — nothing here changes today.

## Decision (2026-11-01, Joshua)
The Waveshare **ESP32-S3-Touch-LCD-3.5** head "will become less than useful."
The likely replacement: a **Waveshare 8.8" DSI Touch display (8.8-DSI-TOUCH-A)**
driven by a **second ESP32-P4** dedicated to the face.
Product: https://www.waveshare.com/8.8-dsi-touch-a.htm

## Why this is a real re-architecture, not a screen swap
The current head is an **ESP32-S3** that is itself a **Bus-A SPI-Slave-HD peer**
of the hub P4 — it renders with LVGL over a small SPI ST7796 panel. The new plan
changes three things at once:
1. **A dedicated ESP32-P4 as the face**, not an S3. The P4 has the MIPI-DSI
   peripheral an 8.8" DSI panel needs (the S3 does not — it drives SPI/RGB
   panels only). So the face MCU changes.
2. **MIPI-DSI panel**, not SPI. A DSI panel is a different display stack
   (esp_lcd_mipi_dsi + a DSI-capable panel driver + a much larger framebuffer in
   PSRAM) than the ST7796-over-SPI adapter the current head uses. The LVGL layer
   on top can largely carry over; the board-adapter layer is new.
3. **Two P4s now** (hub P4 + face P4). The hub↔face link must be reconsidered:
   a P4 face is no longer a natural Bus-A *slave-HD peer* the way the S3 is.
   Options to decide at design time (see below).

## What carries over vs what is new
CARRIES OVER (do not rewrite):
- The `fh_protocol` wire format (frozen, byte-identical everywhere) — PING/PONG
  liveness as health node 4 (`FH_SRC_HEAD`), and the render feed
  (`FH_OD_SYS_STATE_RESP` today; a dedicated head render frame later).
- The LVGL UI intent + the "face renders what the hub feeds, sends taps upstream"
  model from `remix-FH-WS-head-firmware/`.
- The head stays liveness-only / render-only — NOT a capture source.

NEW (must be built):
- A new firmware package (e.g. `remix-FH-P4-head-firmware/`) for the face P4.
- A MIPI-DSI board-adapter layer for the 8.8" panel (replaces `fh_ws_board.cpp`).
- The hub↔face transport decision (below).

## Open design questions (decide before building)
1. **Hub↔face transport.** The S3 head is a Bus-A SPI-Slave-HD peer. A P4 face
   could: (a) stay a Bus-A SPI-Slave-HD peer (P4 can be an HD slave — simplest,
   reuses the frozen 261-byte segment model + `essl_spi_rddma/wrdma` on the hub);
   (b) move to a faster link (P4↔P4 could do higher-rate SPI or even a different
   bus) if the 8.8" UI needs more render bandwidth than the status feed. Default
   to (a) unless a bench shows the feed is too slow.
2. **Does the face P4 keep doing anything besides render?** Keep it render/touch
   only (clean separation, head stays health node 4), unless there's a reason to
   offload work to it.
3. **Power.** An 8.8" DSI panel + backlight draws far more than the 3.5" — budget
   the rail and the connector; this is a hardware-side item.

## 8.8" DSI panel — CONFIRMED specs (Waveshare wiki 8.8-DSI-TOUCH-A, 2026-11-01)
- **Resolution 480×1920 IPS, portrait**, 60 Hz, optical-bonded, 10-point touch.
- **MIPI-DSI, 2-lane** (the wiki: "ESP32-P4-NANO drives the screen via MIPI
  2-lane"). The P4 has the DSI peripheral; the S3 does not — this is why the face
  MCU must change.
- **Display IC: OTA7290B.** ESP-IDF driver is a managed component —
  `idf.py add-dependency "waveshare/esp_lcd_ota7290b"`. Reference BSP:
  waveshareteam `esp32_p4_platform` (`esp32_p4_platform.c`), which shows the full
  DSI bring-up for this exact panel.
- **Touch IC: GT9271** (Goodix, 10-point capacitive, I2C). Pick/confirm its
  esp_lcd_touch driver + I2C address at build time.
- **Backlight: over I2C, NOT a PWM GPIO.** Write 0x00–0xFF to register **0x96**
  of device **0x45**. The `esp32_p4_platform` BSP wraps this as
  `bsp_display_brightness_set(0..100)` / `bsp_display_backlight_on/off()`.
- **Power: 5 V (4.75–5.30 V), input current ≥ 0.43 A** or it fails to start —
  far more than the 3.5" SPI panel; budget the rail + connector.
- **Paired board: ESP32-P4-NANO** (its pin map + the `esp32_p4_platform` BSP are
  the starting point for the face firmware). Drawings (DWG/PDF) + BSP linked on
  the wiki.
- Framebuffer: 480×1920×2 (RGB565) ≈ 1.76 MB per buffer — must live in PSRAM
  (the P4-NANO has PSRAM); plan double-buffer vs single + partial per RAM budget.

## Migration (when it starts)
1. Un-mark PLANNED here (date it) and note it in the hub + head CONTINUITY.
2. Specs are confirmed above — start from the `esp32_p4_platform` BSP + the
   `waveshare/esp_lcd_ota7290b` component rather than writing a DSI driver from
   scratch.
3. Scaffold `remix-FH-P4-head-firmware/` with the frozen `fh_protocol` component
   (byte-identical), the PING→PONG liveness, and the SYS_STATE render feed — same
   contract as `remix-FH-WS-head-firmware/`, so the hub needs no change if the
   face stays a Bus-A HD peer (transport option 1a).
4. Build the MIPI-DSI board adapter on the proven stack: `esp_lcd_ota7290b`
   (OTA7290B, 2-lane DSI, 480×1920) + a GT9271 esp_lcd_touch + I2C backlight
   (reg 0x96 @ dev 0x45, or the BSP's `bsp_display_brightness_set`) + a PSRAM
   framebuffer. Mirror the proven adapter's structure (controller isolated for a
   one-file swap). The `esp32_p4_platform` BSP is the reference bring-up.
5. Keep `remix-FH-WS-head-firmware/` until the P4 face is bench-proven, then
   retire the S3 head (move, don't delete — same policy as the arduino blades).

## UI MODEL — layered head UI (designed 2026-11-01, build on the 3.5" first)

> This UI model is SHARED across every head configuration (3.5" solo, 3.5"+CYD,
> 8.8" DSI). The configs differ only in which glass shows what — see
> `remix-ALTERNATE-CONFIGURATIONS.md`. In the 3.5"+CYD budget config the status
> strip below moves ENTIRELY to the CYD and the Waveshare becomes pure work
> surface.

A navigation redesign for the head, replacing the gen1 "big center menu" with a
LAYERED model. Designed on the **3.5" (480×320)** because Joshua has one on the
bench and the original FootHoled already proved LVGL menus + touch + SD dialogs
run on this panel. This is the **compact half of the resolution-aware plan**: the
same model + data feed later expands to the 8.8" (480×1920) with a wider rail +
content pane — nothing here is throwaway.

### Scope to build (as NEW NextGen UI — reuse proven LVGL patterns, NOT the gen1 flows)
1. The layered shell (status strip + capture rail + ✕ back-stack + content pane).
2. The settings (⚙) screen — a NextGen-native screen, NOT a port of gen1's.
3. The LIVE status bar.

### Layout (3.5" landscape, 480×320)
```
┌─────────────────────────────────────────────────────┐
│ C6/BT● S3/2.4● C5/5● GPS●8   [📡LoRa]   [ ANALYZE ] │  thin strip (~28px)
├──────┬──────────────────────────────────────────────┤
│  ✕   │  CON · live                         [▮ stop]  │  ← default Recon pane
│ CON  │  ───────────────────────────────────────────  │
│  BT  │  2.4  aa:bb… ch6  -42  "Linksys"              │
│  2.4 │  BT   de:ad… ADV   -71  "Tile"                │  consolidated feed:
│  5   │  5    12:34… ch36 -55  "office5G"             │  BT+2.4+5 interleaved,
│ NRF  │  BT   77:88… ADV   -60  "AirPods"             │  each row source-tagged
│ SUB  │  …all continuous feeds merged, tagged…        │
│  ⚙   │  (ALL replaces CON here only in ANALYZE mode) │
└──────┴──────────────────────────────────────────────┘
```

### Top status strip (thin, must not dominate)
- **Health dots (read-only, glance only):** `C6/BT` `S3/2.4` `C5/5` `GPS●<sats>`.
  Labeled by CHIP/BAND, matching the real wire sources — BT = the P4's onboard
  **C6** (hub-local BLE, `FH_SRC_WS_BLE`=5); 2.4 = XIAO-**S3** (`FH_SRC_XS3_WIFI24`
  =2); 5 = XIAO-**C5** (`FH_SRC_C5_WIFI5`=1); GPS = Pico blade (`FH_SRC_PICO`=3).
  Each is an INDEPENDENT health source (NextGen: BT is the C6, not the S3), so
  each lights on its own.
- **NO "Medusa" dot** (that is the TUI's host indicator; on the head the link is
  implied — if the screen draws, the hub+link are up). **NO separate "link" dot**
  for the same reason. (These stay on the hub's own TUI strip; head is tighter.)
- **Tappable `📡LoRa` pill (far-ish right):** indicator + action. Color = LoRa
  link state; tap opens the sensor-stick link overlay FROM ANYWHERE (floats over
  the current screen, own ✕ dismiss, does not disturb capture navigation). LoRa
  and GPS are NOT on the capture rail — GPS supplements recorded data, LoRa is the
  uplink to the sensor sticks; neither is a recon radio.
- **One-word mode button (far right):** an ACTION button, shows the OPPOSITE mode
  — reads "ANALYZE" while in Recon, "RECON" while in Analyze. Tapping flips the
  global mode. Current mode is shown by the PANE (header "<radio> · live/saved" +
  tint), not the button.

### Left capture rail (the "swap bands + show live info" column)
Rail, top to bottom: **✕ · CON · BT · 2.4 · 5 · NRF · SUB · ⚙** (the ordering
itself teaches the model: overview → the continuous bands it merges → the fired
radios → settings). The **✕** is back one level / to home.
- `CON` → **CONSOLIDATED live feed: BT + 2.4 + 5 merged into one pane**, each row
  source-tagged (`BT`/`2.4`/`5`). The **default Recon pane** — you land here (the
  overview) and drill into a single band when you want to focus. Continuous-only
  by definition (it merges the always-on radios); NRF/SUB are not in it.
- `BT` → C6 BLE (hub-local, talks to `fh_hub_ble`)
- `2.4` → S3 2.4 GHz Wi-Fi (Bus-A peer)
- `5`  → C5 5 GHz Wi-Fi (Bus-A peer)
- `NRF` → nRF24L01+ 2.4 GHz capture (CC1101-style on-demand blade) — the Nordic/
  ShockBurst PHY, a DIFFERENT radio from `2.4` Wi-Fi on the same band.
- `SUB` → sub-GHz / CC1101 OOK (on-demand blade).
- `⚙`  → settings (below).
- `ALL` → ANALYZE mode only: cross-radio SAVED rollup (wardrive / full recon log
  / all devices). CON (Recon, live merge) and ALL (Analyze, saved rollup) are the
  two-mode forms of "everything together" but kept as SEPARATE names so each
  means exactly one thing.

### The main pane has three personalities (the rail tells you which)
1. **CON — consolidated overview (default Recon).** Merged live feed of the three
   continuous radios; a `[stop]`-style control + the rolling tagged list.
2. **BT / 2.4 / 5 — single continuous feed (focus).** The feed IS the pane, no
   drill-in; a thin start/stop control row above it. Switching bands swaps which
   feed the pane shows — the others keep running (strip dots + CON prove it).
3. **NRF / SUB — "one of a kind" fired-capture panes.** NOT a feed you switch
   into: they lead with a fire control ("capture N s ▸"), show "idle — fire a
   capture" when nothing is running, then fill as the dump streams back. They
   must LOOK different from the continuous panes so the difference needs no
   explanation. Self-contained; not part of the CON merge.
Mental model: CON is the lobby, BT/2.4/5 are focus rooms off it, NRF/SUB are
separate workshops. ✕ walks back toward CON/home.

### RECON vs ANALYZE (global mode)
- **RECON:** default pane is **CON** (consolidated overview); from there tap a
  band to focus or NRF/SUB to fire a capture. "Go collect." (Pane shapes: see
  "three personalities" above — continuous feed vs fired capture.)
- **ANALYZE:** tap a radio → its SAVED views (device list, dupe-ssid, replay);
  the top rail slot is `ALL` (cross-radio saved rollup) instead of CON.
- **Flipping mode NEVER stops a running capture** — captures run on the blades/
  peers independent of what the face shows; the strip keeps showing them live,
  and CON keeps merging them.

### Settings (⚙)
A NEW NextGen screen (gen1's local-NVS settings service is NOT ported — see the
protocol note). Surfaces the hub's existing config (`fh_hub_config`: GPS rate,
brightness, BLE scan mode, relog mode/window, band mask, Wi-Fi dwell — already in
`SYS_STATE`). SD-format flow + About can be NextGen-native screens here, built to
this design (reuse the gen1 LVGL PATTERNS — two-stage destructive confirm,
worker-task progress bar — not its code). The LoRa stick-link manager lives
behind the top pill (or here).

### Protocol dependencies (honest — what's live now vs. what a frame must add)
- **LIVE NOW (head just renders `SYS_STATE`):** GPS fix + sat count; the config
  values on the settings screen (DISPLAY only).
- **CON needs NO new protocol** — it is a HEAD-SIDE merge of capture events the
  hub already streams tagged by src_id (the logger + Medusa already tell radios
  apart this way). CON = render all continuous-source events into one list
  instead of filtering to the selected radio. One of the CHEAPER items here.
- **NEEDS the hub to feed per-node health to the head:** the C6/BT, S3/2.4, C5/5
  dots. The hub already KNOWS these (it drives the TUI's identical strip) but
  `SYS_STATE` does not carry per-node up/down yet. This is exactly what the parked
  "dedicated head render frame" (hub CONTINUITY §4) is for — this UI justifies it.
  Either extend `SYS_STATE` with the 4–5 node states or add the render frame.
- **NEEDS a new config-WRITE verb** for head-side settings EDITING (head→hub "set
  brightness=80"). Until it exists: head DISPLAYS settings (works now), Medusa
  EDITS them. Small, clean addition to the frozen protocol when wanted.
- **LoRa stick-link protocol EXISTS (NOT a dependency to design)** — it is the
  FROZEN, build-agnostic FootHoled-Sensor-Stick handoff contract (GET/HELLO/
  chunks/END/DONE, CRC32-safe, LoRa `D,<seq>`/`A,<seq>` framing, fixed BLE UUIDs,
  915 MHz). The deck is the COLLECTOR (SX1262 blade + C6 BLE); the overlay's
  registry is keyed by each stick's `NODE_ID`; the deck INITIATES pickups. Full
  record: `remix-SENSOR-STICK-COLLECTOR.md`. Remaining work is COLLECTOR
  IMPLEMENTATION, not protocol design.
- Mode button + ✕ back-stack are LOCAL head UI state — no protocol needed.

### Resolution-aware note
Build the LVGL layout so the SAME model + data feed renders compact on 480×320
(this 3.5" experiment) and expanded on the 8.8" 480×1920 (persistent rail + wider
content, ~6× the list rows). One data model, two layouts — the 3.5" work is the
compact half, not throwaway.

## Note
The current `remix-FH-WS-head-firmware/` (ESP32-S3-Touch-LCD-3.5) remains the
head of record until the above is built and bench-proven. This file is the plan,
not a change to today's code.
