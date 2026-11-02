# AGENTS.md — remix-FH-WS-head-firmware (for AI / LLM agents)

Read before editing. Root `../AGENTS.md` governs the rig; the hub
`../remix-FH-P4-hub-firmware/CONTINUITY.md` and
`../remix-BUS-A-QUAD-HD-DECISION.md` govern Bus A. This file is head-specific.

## What this is
The Waveshare ESP32-S3-Touch-LCD-3.5 (NON-B) HEAD. Display/touch/power = a
FAITHFUL PORT of the PROVEN original FootHoled board adapter (bench-working on
this board). REMIX adds a Bus-A SPI-Slave-HD peer link, PING/PONG liveness, and
an LVGL status UI fed by the hub.

## PROTECTED BASELINE — do not "improve" the board adapter
`fh_ws_board.cpp` mirrors `JoshuaStanden/FootHoled@josh-dev board_adapter.cpp`,
which has a documented regression history (`firmware/DISPLAY_NOTES.md`). The
following are LOAD-BEARING; changing any of them reintroduces a blank screen:
1. **TCA9554 reset pulse BEFORE panel init.** LCD/touch reset are on the
   expander (GPIO_NUM_NC on the SoC). Pulse expander pin 1 low(100ms)→high
   before `esp_lcd_panel_reset/init`, or the panel is held in reset and comes up
   blank while every `esp_lcd_*` returns ESP_OK.
2. **PSRAM = OCTAL.** The S3R8 is octal PSRAM. Do NOT set `SPIRAM_MODE_QUAD`
   (the "8MB PSRAM" text misleads; QUAD fails PSRAM init at boot → display dies).
3. **Draw buffers in internal DMA RAM** (`MALLOC_CAP_DMA|MALLOC_CAP_INTERNAL`)
   with the `SPIRAM_MALLOC_RESERVE_INTERNAL=131072` reserve. S3 SPI-DMA can't
   read PSRAM; a PSRAM source forces a bounce buffer that starves.
4. **One `esp_lcd_panel_mirror(true,false)` in init, never on the flush path.**
5. Pins are FIXED: LCD MOSI=1 SCLK=5 DC=3 BL=6(LEDC) CS/RST=NC; I2C SDA=8 SCL=7;
   on SPI2. These are the board's, not a choice.

## REMIX non-negotiables
6. **Bus A is SPI Slave HD, quad, mode 0, cmd/addr/dummy=8, 261 B — FROZEN
   bus-wide** (`../remix-BUS-A-QUAD-HD-DECISION.md`). On the head it is on
   **SPI3** because SPI2 is the LCD (R-SPIHOST). The link contract
   (pop/on_control) matches the XIAO peers.
7. **The head is FH_SRC_HEAD=4 — liveness only, NO capture.** Answers PING→PONG;
   never emits CAPTURE/SUMMARY/SSID.
8. **1-bit HD before QIO** on Bus A (dummy-cycle #9823).

## VERIFY items (not yet bench-checked by REMIX)
- The Bus-A GPIOs (fh_ws_pins.h, SPI3) are expansion-header placeholders —
  confirm each is free (not display/I2C, not strapping 0/3/45/46).
- Touch coord swap/mirror per rotation — `read_touch_raw` applies none; the
  robust fix is the `esp_lcd_touch_ft6336` component (DISPLAY_NOTES open item).
- The REMIX link/UI code is reasoned, not bench-run. The board adapter is a
  copy of a proven path but was not re-run under REMIX.

## Review discipline
Subject to `../remix-MULTI-CHECK.md`. The board adapter is a protected baseline:
new features live in their own files and must not alter its display init. Docs
travel with the edit.
