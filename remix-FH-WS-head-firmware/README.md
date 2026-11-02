# remix-FH-WS-head-firmware — Waveshare HEAD (the face)

> Waveshare **ESP32-S3-Touch-LCD-3.5 (NON-B)** as the FootHoled **HEAD**. In
> REMIX the head is demoted from hub to FACE: it renders what the P4 hub feeds
> it and reports taps upstream. Transport is a **Bus-A SPI Slave HD** peer
> speaking the binary **fh_protocol**.

**Docs:** `WIRING.md` · `README.md` (this) · `AGENTS.md` · `CONTINUITY.md` · `FUTURE.md`.

## Architecture (Option A — reuse the proven board driver)
The display/touch/power layer is a **faithful port of the original FootHoled
board adapter** (`JoshuaStanden/FootHoled @ josh-dev`,
`firmware/src/board_adapter.cpp` + `DISPLAY_NOTES.md`), which is **bench-working
on this exact board**. We did NOT hand-roll a new ST7796/FT6336 driver — that
would fight the hardware (the LCD/touch reset lines are on a TCA9554 expander,
the backlight is LEDC PWM, PSRAM is octal). Reusing the proven path is correct
and fastest, and `josh-dev` already proves it on this screen.

Pieces:
1. **`fh_ws_board.cpp`** — proven board adapter: ST7796 SPI (SPI2@80MHz) +
   TCA9554 reset pulse + FT6336 I2C touch + LVGL 9.2 display/indev + LEDC
   backlight. Internal-DMA draw buffers, OCTAL PSRAM. (See WIRING / DISPLAY_NOTES.)
2. **`fh_ws_ui.c`** — LVGL status screen (uptime / SD / GPS / jobs / cfg) fed by
   the hub; a tap fires the upstream callback.
3. **`fh_ws_link.c`** — Bus-A SPI Slave HD peer on **SPI3** (SPI2 is the LCD);
   receives the hub's frames, sends PONG + taps upstream.
4. **`fh_ws_main.c`** — app entry: board init → UI → Bus-A link → LVGL loop.
   Answers PING→PONG (lights hub health node 4 / Medusa Display green), maps the
   hub's `FH_OD_SYS_STATE_RESP` onto the UI.

## The render feed (honest state)
The hub's *dedicated* head render frame is still parked (hub CONTINUITY §4).
Until it's frozen, the head renders from the hub's existing
`FH_OD_SYS_STATE_RESP` (0x2E). When the hub gains a dedicated render frame, add
one `case` in `fh_ws_main.c` `link_on_control()`; nothing else changes.

## Build / flash (your hardware)
ESP-IDF 6.1.x, LVGL 9.2.x, target esp32s3:
```bash
idf.py set-target esp32s3
idf.py build
idf.py -p <port> flash monitor
```
Managed components (`lvgl`, `esp_lcd_st7796`, `esp_io_expander_tca9554`) resolve
from `main/idf_component.yml`. PSRAM is OCTAL in `sdkconfig.defaults` — do not
change it to QUAD (that kills the display; see WIRING).

## Status
Board half = faithful copy of a path proven on this board. REMIX half (Bus-A HD
link + PING/PONG + UI feed) is new, reasoned-not-bench. Risk surface: the Bus-A
HD/QIO transport (1-bit HD first, #9823) and the Bus-A header-GPIO VERIFY items.
See `CONTINUITY.md` / `FUTURE.md`.
