# AGENTS.md — remix-FH-XIAO-S3-firmware (for AI / LLM agents)

Read this BEFORE editing the S3. The repo-root `../AGENTS.md` governs the whole
rig; this file carries the S3-specific lore. Five-doc set: WIRING / README /
AGENTS (this) / CONTINUITY / FUTURE.

## Non-negotiables (this peer)
1. **This is an ESP32 Bus-A peer, a quad SPI SLAVE HD at MODE 0** (`spi_slave_hd`,
   `sc.mode = 0`). The RP2040 MODE-1 one-byte-per-CS rule does **NOT** apply here
   — that is a Pico-blade concern. Root `../AGENTS.md` §1: "Bus A peers
   (ESP32-family, not RP2040) stay MODE 0." Do NOT change this to MODE 1 and do
   NOT claim the MODE-1 rule applies to this board.
2. **Bus A = quad SPI Slave HD, FROZEN** (`../remix-BUS-A-QUAD-HD-DECISION.md`).
   The ordinary full-duplex `spi_slave` driver is 1-bit only, so quad requires
   `spi_slave_hd` (half-duplex, segment mode): TX chan = staged frame the hub
   pulls with `essl_spi_rddma`; RX chan = control the hub pushes with
   `essl_spi_wrdma`. FROZEN params (must match the hub + the other peers): mode 0,
   command/address/dummy = 8 bits, 261-byte segments. Do NOT revert to
   `spi_slave`. **#1 bench item:** bring up 1-bit HD first, then QIO and align
   dummy cycles (Espressif #9823) — `fh_xs3_link.c` L2.
3. **CS/IRQ on GPIO43/44 = the S3's UART0 console pins.** Usable here (console is
   on USB-CDC; these carry SPI CS + a GPIO IRQ, not a UART), but NOT risk-free:
   verify boot-time UART0 activity doesn't glitch CS/IRQ. If it does, move both to
   SPI-unused free pins and update the hub side (carried ABYSS-S3-1).
4. **Capture callback NEVER blocks.** The promiscuous cb does a 0-tick try-lock
   on the seen-table + 0-tick enqueue (drop+count on contention/full). NO
   transport I/O in the callback. The SPI Slave HD link task is the ONLY transport
   path; it drains the queue and stages the next frame, raising IRQ. Do not do
   SPI I/O from the Wi-Fi callback.
5. **Capture path is carried VERBATIM from the proven gen1 firmware** — only the
   transport moved (UART → SPI Slave HD + IRQ). The risk surface is `fh_xs3_link.c`,
   not the capture logic. The SSID mapping (one `FH_TYPE_SSID` per BSSID, on first
   sight + on SSID change) uses a separate table + mutex; it does not touch the
   capture path. `src_id = FH_SRC_XS3_WIFI24 (2)`, band nibble 0x2.

## Known bench risks (cannot be resolved by reading)
- QIO dummy-cycle alignment with the hub (Espressif #9823) — #1; bring up 1-bit
  HD first (`fh_xs3_link.c` L2).
- UART0-pin glitch on CS/IRQ at boot (above).
- `spi_slave_hd` on SPI2_HOST; DMA-capable buffers, RX length 4-byte aligned
  (264 B for a 261-byte segment); DQ2/DQ3 = GPIO1/GPIO2 placeholders — confirm
  free pins + IDF 6.1 behavior (`fh_xs3_link.c` L3/L4).
- 2.4 GHz channel list vs your regulatory domain (1..11 vs 1..13).

## Review discipline
Subject to `../remix-MULTI-CHECK.md`. Kiro never compiles/flashes here. Docs
travel with the edit: a transport change updates this file + `CONTINUITY.md`.
