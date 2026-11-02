# CONTINUITY.md — remix-FH-WS-head-firmware (resume point)

Snapshot for picking the head back up. Pair with `AGENTS.md`, `WIRING.md`,
`FUTURE.md`, `../remix-BUS-A-QUAD-HD-DECISION.md`.

## Where this stands (2026-11-01)
- **FUTURE DIRECTION (planned, not started):** this ESP32-S3-Touch-LCD-3.5 head
  is expected to be replaced by an 8.8" MIPI-DSI panel driven by a dedicated
  SECOND ESP32-P4 — see `../remix-HEAD-ROADMAP.md`. This S3 head remains the head
  of record until that is built and bench-proven; keep maintaining it.
- The head firmware exists. **Option A adopted** (user's call): reuse the PROVEN
  original-FootHoled board adapter for display/touch/power, rather than
  hand-rolled bare-metal drivers. The user confirmed `JoshuaStanden/FootHoled@
  josh-dev` already works on this exact board.
- **Course correction this session:** the first draft hand-rolled a direct-GPIO
  ST7796/FT6336 driver + assumed quad PSRAM. Fetching the Waveshare wiki + the
  josh-dev firmware showed that was wrong: the LCD/touch reset is on a TCA9554
  expander, the backlight is LEDC PWM, and the R8 PSRAM is OCTAL (not quad). The
  bare-metal files were deleted and replaced with a faithful port of the proven
  adapter. The quad-PSRAM "fix" was reverted to OCT.

## Files
- `main/fh_ws_board.cpp/.h` — PROVEN board adapter port (ST7796 SPI2@80MHz +
  TCA9554 reset + FT6336 I2C + LVGL 9.2 + LEDC backlight, internal-DMA buffers).
- `main/fh_ws_ui.c/.h` — LVGL status screen + tap callback.
- `main/fh_ws_link.c/.h` — Bus-A SPI-Slave-HD peer on **SPI3** (twin of the XIAO
  HD links; SPI2 is the LCD).
- `main/fh_ws_main.c` — board init → UI → link → LVGL loop; PING→PONG, SYS_STATE→UI.
- `main/fh_ws_pins.h` — Bus-A (SPI3) placeholder pins only; LCD/I2C pins live in
  the adapter (one source of truth, proven values).
- `main/idf_component.yml` — lvgl 9.2, esp_lcd_st7796, esp_io_expander_tca9554.
- `components/fh_protocol/` — byte-identical shared protocol.
- `CMakeLists.txt` ×2, `sdkconfig.defaults` (esp32s3, OCT PSRAM, LVGL Kconfig,
  internal-DMA reserve).

## Decisions locked in (do not revert)
- Display/touch/power = proven adapter (Option A). Protected baseline — see
  AGENTS §"PROTECTED BASELINE" (TCA9554 pulse, OCT PSRAM, internal-DMA buffers,
  single mirror, fixed pins).
- Bus A = SPI Slave HD, quad, mode 0, 261 B, frozen bus-wide. Head's Bus-A host
  = SPI3 (SPI2 is the LCD; R-SPIHOST).
- Head = FH_SRC_HEAD=4, liveness only, no capture.
- Render feed = SYS_STATE (0x2E) until the hub's dedicated render frame lands.

## What is NOT done (bench + follow-ups)
- The board adapter is a faithful copy of a proven path but was NOT re-run under
  REMIX. The REMIX link + UI are reasoned, not bench-run.
- Bus-A HD: 1-bit HD first, then QIO (#9823). Hub master half (ESSL) is in
  `../remix-FH-P4-hub-firmware/main/fh_hub_link.c`.
- Bus-A GPIOs (SPI3) are header placeholders — VERIFY on the board.
- Touch coord swap/mirror per rotation — DISPLAY_NOTES open item.
- The hub's dedicated head render frame is parked; SYS_STATE is the stand-in.

## Next session should
1. Bench the display alone (no hub) — TCA9554 pulse + "waiting for hub..." proves
   the proven adapter ported cleanly.
2. Then the Bus-A link against the hub (SPI3, 1-bit HD first → PING→PONG → node-4
   green).
3. When the hub gains a render frame, add its case in `link_on_control`.

## Multi-check (2026-11-01) — PASS, one fix applied
- **Board-port fidelity vs josh-dev board_adapter.cpp: PASS.** All pins/params/
  backlight constants byte-match the original (MOSI1/SCLK5/DC3/BL6/SDA8/SCL7,
  320×480, 80MHz, SPI2, LEDC TIMER_1/CH0/10-bit/5kHz, ceil 85%). Init SEQUENCE
  matches exactly: SPI→I2C→TCA9554 pin1 assert(100)/release(100)→panelIO→ST7796
  reset/init/invert(true)/mirror(true,false)/disp_off→touch→LVGL(internal-DMA +
  PSRAM fallback)→disp_on→indev→backlight.
- **LVGL lifecycle vs proven main.cpp: PASS after fix.** Ordering lv_init →
  board_init → UI → loop matches. FIXED two bugs the proven loop avoids:
  (1) tick drift — was `last=now` each pass (discards sub-ms remainder); now
  accumulates only whole ms (`last += elapsed*1000`). (2) `lv_timer_handler` +
  UI build were ungated; now gated on `display_ready` (no fault / no LVGL calls
  on a failed-display boot). Link still starts headless so PONG/node-4 survives
  a panel failure.
- **ESP-IDF spi_slave_hd API vs the real header (v5.3→master): PASS.**
  `spi_slave_hd_slot_config_t` fields (mode/spics_io_num/flags/command_bits/
  address_bits/dummy_bits/queue_size/dma_chan/cb_config) all correct; 3-arg
  `spi_slave_hd_init(host,bus,slot)` correct; queue_trans/get_trans_res sigs
  correct; `spi_slave_hd_data_t` .data/.len/.trans_len/.arg correct. `flags=0`
  = 1-bit HD = the intended first-bring-up state (widen to quad is step 2).
- **Protocol field names vs fh_ondemand.h: PASS.** uptime_s/sd_mounted/
  sd_free_mb/fix_valid/sats/jobs_done/job_timeouts/cfg_modified/override_count
  are real fields of the 74-B fh_od_sys_state_resp_t; PING/PONG = fh_ping_t{seq};
  fh_frame_cb_t sig matches in_cb/link_on_control.
- **Residual (noted, not fixed):** the TX/RX descriptors are `static` locals —
  fine for the single-outstanding-each-way design (queue_size=4 leaves room),
  but a future 2nd-in-flight queue would need distinct descriptors. The 74-B
  s_state struct-copy race is tolerated (one cosmetically-torn frame max).
