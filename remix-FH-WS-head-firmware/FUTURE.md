# FUTURE.md — remix-FH-WS-head-firmware (what's next)

Forward notes for the head. Pair with `CONTINUITY.md`, `AGENTS.md`, and the
Bus-A decision doc.

## Bench-gated (prove on hardware — in order)
1. **Display alone (no hub).** The proven adapter port should show
   "FootHoled HEAD / waiting for hub...". Blank screen → the two historical
   causes: (a) TCA9554 reset pulse didn't run, (b) PSRAM not OCT. Both are
   locked in; this step confirms the port didn't drift.
2. **Touch.** Tap registers in LVGL. Coord swap/mirror per rotation may need
   tuning — the robust fix is the `esp_lcd_touch_ft6336` component (the proven
   adapter uses an inline register read that applies no swap/mirror).
3. **Bus-A link (SPI3).** 1-bit HD first → ESSL Rd_DMA/Wr_DMA + PING→PONG →
   Medusa node-4 green. THEN QIO (align dummy cycles, #9823).
4. **Bus-A GPIOs.** Confirm the SPI3 placeholder pins are free header pins.

## Framework extensions (hooks already in place)
- **Dedicated hub render frame.** When the hub freezes a head render-frame type
  (hub CONTINUITY §4), add a `case` in `fh_ws_main.c link_on_control()`. The
  feed today is SYS_STATE (0x2E).
- **Real touch-event frame.** A tap currently nudges the hub with a sentinel
  (PING seq 0xFE). Define a touch-event frame (x/y/gesture) in `fh_ondemand.h`
  when the UI needs the hub to act on taps. `on_tap` already has x/y.
- **Richer LVGL UI.** The status screen is minimal labels. LVGL 9.2 is fully
  available (flex, buttons, slider, switch, image per the Kconfig set) — build
  real screens on `fh_ws_ui.c` without touching the board adapter.
- **IMU auto-rotate.** The proven adapter has a QMI8658 auto-rotate service we
  trimmed (it coupled to the original's settings store). The IMU is on the
  shared I2C bus (`fh_ws_board_i2c_bus()`); re-add it if the head should rotate.

## Explicitly rejected / do not do
- Do NOT replace the proven adapter with hand-rolled direct-GPIO drivers (the
  first draft did; it was wrong — reset is on the TCA9554, PSRAM is octal).
- Do NOT set `SPIRAM_MODE_QUAD` (kills the display).
- Do NOT move the LCD off SPI2 or the Bus-A slave onto SPI2 (R-SPIHOST).
- Do NOT make the head a capture source (liveness-only, FH_SRC_HEAD=4).
- Do NOT start Bus A at QIO (1-bit HD first).
