# remix-FH-XIAO-S3 — WIRING

> Seeed **XIAO ESP32-S3**, 2.4 GHz Wi-Fi sniffer, now a **SPI SLAVE** on the P4
> hub's Bus A. GPIO numbers are the documented XIAO-S3 silkscreen→GPIO map —
> **VERIFY each against YOUR board** at bench. Nothing here was flashed on the
> authoring machine.

## SPI-slave link to the P4 hub (Bus A)

| XIAO-S3 pin | GPIO | Signal | Hub side (P4) |
|---|---|---|---|
| D8  | GPIO7  | SCK  (hub drives) | FH_BUSA_CLK = GPIO2 |
| D9  | GPIO8  | MISO (S3 → hub)   | FH_BUSA_DQ1 (this peer's read lane) |
| D10 | GPIO9  | MOSI (hub → S3)   | FH_BUSA_DQ0 (this peer's write lane) |
| D7  | GPIO44 | CS   (hub selects)| FH_BUSA_CS_XS3 = GPIO22 |
| D6  | GPIO43 | IRQ  (S3 → hub)   | FH_BUSA_IRQ_XS3 = GPIO29 |

- **IRQ idle LOW, HIGH = "I have a frame."** Hub reads it rising-edge.
- **★ GPIO43/44 are the S3's UART0 console pins** — usable here (console is on
  USB-CDC, these carry SPI CS + GPIO IRQ, not a UART) but **not risk-free**:
  verify boot-time UART0 activity doesn't glitch CS/IRQ. If it does, move both
  to SPI-unused free pins and update the hub side. (See `main/fh_xs3_pins.h`.)
- **Bus A is quad SPI Slave HD** (`spi_slave_hd`, segment mode — committed, see
  `../remix-BUS-A-QUAD-HD-DECISION.md`). All four data lanes are wired: DQ0
  (MOSI), DQ1 (MISO), DQ2 = `FH_XS3_SPI_WP` and DQ3 = `FH_XS3_SPI_HD` (GPIO1 /
  GPIO2 — **placeholders, VERIFY free pins on your board**). Bring up **1-bit HD
  first**, then widen the hub to QIO and align dummy cycles (Espressif #9823) —
  the #1 bench item, `fh_xs3_link.c` L2.

## Power / ground / console
- Common **ground** between the XIAO-S3 and the P4 — matters more than clock.
- 3V3 to the XIAO (from the hub rail or its own USB-C).
- Console + flashing: **USB-C** (USB-Serial-JTAG). One cable.

## Bench order
1. Flash standalone; confirm 2.4 GHz promiscuous RX + the heartbeat console line.
2. Wire the SPI slave + IRQ to the hub; prove ONE frame moves (hub logs a
   capture row) before adding the C5 peer.
