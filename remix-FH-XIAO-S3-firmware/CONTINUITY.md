# remix-FH-XIAO-S3 — CONTINUITY (future Kiro + Joshua)

> If a session is lost, this file lets us resume the XIAO-S3 work without
> re-deriving it. Snapshot: 2026-10-25. Folder: `~/Desktop/remix footholed/`.
> Standing rules (full set in the P4 hub's CONTINUITY.md): **no compile/flash on
> this machine**, remix- prefix, whiteboard-by-default, honest deep review.

## What this is
Seeed **XIAO ESP32-S3** — pure **2.4 GHz Wi-Fi promiscuous sniffer**. In REMIX
it is a **SPI SLAVE** on the P4 hub's Bus A (was a UART node in gen1).

## What changed vs gen1 (the port)
- **Capture path: UNCHANGED** (promiscuous cb → seen-table dedup → TX queue →
  CAPTURE/SUMMARY/HEARTBEAT). This is the proven gen1 logic, carried verbatim.
- **SSID mapping added 2026-10-31 (concern #3):** beacons/probe-resps now emit
  one FH_TYPE_SSID (fh_ssid_t, 39 B) per BSSID — on first sight, again only if
  the SSID changed (FNV-1a hash compare). Separate 128-entry SSID table +
  mutex; capture path untouched. Hidden APs (SSID tag len 0) still emit
  (ssid_len 0 = hidden, which the hub renders "ssid=<hidden>"). Queue-full
  reverts the entry so the next beacon retries.
- **Transport: UART → SPI slave.** New file `fh_xs3_link.c` replaces the UART
  `tx_task`. It stages the next queued frame into the slave transaction buffer,
  raises `FH_XS3_IRQ_OUT` ("come clock me"), and the hub's transaction clocks
  the frame out + any control frame in. Fixed-size 261 B transactions (matches
  the hub master). Inbound control (PING→PONG) decoded in-link.
- **Pins: `fh_xs3_pins.h`** — SPI slave on D8/D9/D10 (GPIO7/8/9) + CS GPIO44 +
  IRQ GPIO43.

## Files
`main/`: `fh_xs3_main.c` (capture, unchanged), `fh_xs3_link.{h,c}` (new SPI
transport), `fh_xs3_pins.h`. Plus `components/fh_protocol/` (byte-identical),
CMake ×2, `sdkconfig.defaults`, `README.md`, `WIRING.md`, this file.

## Open BENCH items (cannot resolve by reading)
1. **QIO dummy-cycle alignment** (fh_xs3_link.c L2, Espressif #9823): Bus A is
   quad SPI Slave HD (committed). Bring up 1-bit HD first, then widen the hub to
   QIO and align dummy cycles. **#1 bench item.** (The old "quad master vs 1-bit
   slave" seam is RESOLVED — the link was rewritten to `spi_slave_hd`.)
2. **CS/IRQ on GPIO43/44** = S3 UART0 console pins. Usable (console on USB-CDC)
   but verify no boot-time UART0 glitch on CS/IRQ. Move + update hub if it bites.
3. `spi_slave_hd` on SPI2_HOST; DMA-capable buffers, RX length 4-byte aligned
   (264 B); DQ2/DQ3 = GPIO1/GPIO2 placeholders. Confirm on IDF 6.1 + the board.
4. 2.4 GHz channel list vs your regulatory domain (1..11 vs 1..13).

## Not done / later
- The hub-side master + the C5 peer must exist to prove a frame moves end-to-end
  (the hub firmware is done; the C5 is its twin, done alongside this).
