# remix-FH-XIAO-C5 — CONTINUITY (future Kiro + Joshua)

> If a session is lost, this resumes the XIAO-C5 work. Snapshot: 2026-10-25.
> Folder: `~/Desktop/remix footholed/`. Standing rules (full set in the P4 hub's
> CONTINUITY.md): **no compile/flash on this machine**, remix- prefix,
> whiteboard-by-default, honest deep review.

## What this is
Seeed **XIAO ESP32-C5** — pure **5 GHz Wi-Fi promiscuous sniffer** (new
silicon). In REMIX it is a **SPI SLAVE** on the P4 hub's Bus A (was UART in gen1).
It is the TWIN of the XIAO-S3 — identical structure, differing ONLY by band.

## What changed vs gen1 (the port)
- **Capture path: UNCHANGED** from gen1 (promiscuous cb → seen-table → queue →
  CAPTURE/SUMMARY/HEARTBEAT), carried verbatim.
- **SSID mapping added 2026-10-31 (concern #3):** same as the S3 (beacons +
  probe-resps emit one FH_TYPE_SSID per BSSID, on first sight + on SSID
  change; hidden APs emit ssid_len 0). Separate 128-entry table + mutex;
  capture path untouched.
- **Transport: UART → SPI slave.** `fh_c5_link.c` (identical to the S3's link,
  renamed) — stages a frame, raises `FH_C5_IRQ_OUT`, hub clocks it; fixed-size
  261 B; inbound PING→PONG decoded in-link.
- **Pins: `fh_c5_pins.h`** — SPI slave on D8/D9/D10 (GPIO8/9/10) + CS GPIO12 +
  IRQ GPIO11.

## The ONE real delta vs the S3 (do not lose this)
5 GHz needs `esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY)` and a 5 GHz channel
list `{36,40,44,48,149,153,157,161}` (non-DFS), band nibble **0x5**, src_id
**FH_SRC_C5_WIFI5 (1)**. Everything else is the S3 firmware.

## Files
`main/`: `fh_c5_main.c` (capture + 5 GHz init), `fh_c5_link.{h,c}`,
`fh_c5_pins.h`. Plus `components/fh_protocol/` (byte-identical), CMake ×2,
`sdkconfig.defaults`, `README.md`, `WIRING.md`, this file.

## Open BENCH items (cannot resolve by reading)
1. **5 GHz on new silicon**: confirm `esp_wifi_set_band_mode` exists on IDF 6.1
   and 5 GHz promiscuous RX works. Start 20 MHz / WIFI_SECOND_CHAN_NONE; verify
   HE (11ax) RX before widening (IDF 6.0 HE/secondary-channel quirk; 6.1 TBD).
   **Predicted first failure.**
2. **QIO dummy-cycle alignment** (fh_c5_link.c L2, Espressif #9823): Bus A is
   quad SPI Slave HD (committed); bring up 1-bit HD first, then QIO. (The old
   "quad master vs 1-bit slave" seam is RESOLVED — link rewritten to
   `spi_slave_hd`.)
3. `spi_slave_hd` on SPI2_HOST; RX length 4-byte aligned (264 B); DQ2/DQ3 =
   GPIO1/GPIO2 placeholders. Confirm on IDF 6.1 + the board.
4. 5 GHz channel list legality / DFS for your regulatory domain.

## Not done / later
- End-to-end needs the hub master (done) + at least one peer proven; bring the
  S3 up first (mature silicon), then the C5 (new-silicon risk).
