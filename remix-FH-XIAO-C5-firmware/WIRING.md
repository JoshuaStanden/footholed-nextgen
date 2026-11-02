# remix-FH-XIAO-C5 — WIRING

> Seeed **XIAO ESP32-C5**, 5 GHz Wi-Fi sniffer, now a **SPI SLAVE** on the P4
> hub's Bus A. GPIO numbers are the documented XIAO-C5 silkscreen→GPIO map —
> **VERIFY each against YOUR board** at bench. New silicon: needs ESP-IDF 6.1.
> Nothing here was flashed on the authoring machine.

## SPI-slave link to the P4 hub (Bus A)

| XIAO-C5 pin | GPIO | Signal | Hub side (P4) |
|---|---|---|---|
| D8  | GPIO8  | SCK  (hub drives) | FH_BUSA_CLK = GPIO2 |
| D9  | GPIO9  | MISO (C5 → hub)   | FH_BUSA_DQ1 (this peer's read lane) |
| D10 | GPIO10 | MOSI (hub → C5)   | FH_BUSA_DQ0 (this peer's write lane) |
| D7  | GPIO12 | CS   (hub selects)| FH_BUSA_CS_XC5 = GPIO23 |
| D6  | GPIO11 | IRQ  (C5 → hub)   | FH_BUSA_IRQ_XC5 = GPIO30 |

- **IRQ idle LOW, HIGH = "I have a frame."** Hub reads it rising-edge.
- Unlike the XIAO-S3, the C5's CS/IRQ (GPIO12/11) are **not** UART0 console pins
  — no boot-glitch concern here, but still VERIFY free on the board.
- **Bus A is quad SPI Slave HD** (`spi_slave_hd`, segment mode — committed, see
  `../remix-BUS-A-QUAD-HD-DECISION.md`). The C5 DOES have the HD slave
  peripheral (ESP-IDF `SOC_SPI_SUPPORT_SLAVE_HD_VER2` = 1 for esp32c5). All four
  data lanes are wired: DQ0 (MOSI), DQ1 (MISO), DQ2 = `FH_C5_SPI_WP` and DQ3 =
  `FH_C5_SPI_HD` (GPIO1 / GPIO2 — **placeholders, VERIFY free pins**). Bring up
  **1-bit HD first**, then QIO (Espressif #9823) — `fh_c5_link.c` L2.

## 5 GHz capture note (the C5's one real delta)
- Firmware calls `esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY)`. Start at
  20 MHz / `WIFI_SECOND_CHAN_NONE`, **verify HE (11ax) RX** before widening.
- Starter channel set is non-DFS: `36,40,44,48,149,153,157,161`. DFS channels
  need radar-detect handling (not done).

## Power / ground / console
- Common **ground** between the XIAO-C5 and the P4.
- 3V3 to the XIAO. Console + flashing: **USB-C** (USB-Serial-JTAG).

## Bench order
1. Flash standalone; confirm **5 GHz** symbols exist in your IDF 6.1 and HE RX
   works (the predicted first failure for new silicon) + the heartbeat line.
2. Wire the SPI slave + IRQ to the hub; prove a frame moves after the S3 peer
   is already proven.
