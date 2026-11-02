# remix-FH-XIAO-S3-firmware — 2.4 GHz Wi-Fi sniffer (SPI slave)

> Seeed **XIAO ESP32-S3**. Pure 2.4 GHz Wi-Fi promiscuous sniffer. In REMIX it
> is a **SPI SLAVE** on the P4 hub's Bus A (was a UART node in gen1). The
> **capture logic is unchanged** from the proven gen1 firmware — only the
> transport moved (UART → SPI slave + IRQ). Written by reading only; NOT
> compiled/flashed here. Target: **esp32s3**, ESP-IDF 6.1.

## What it does
- esp_wifi promiscuous on 2.4 GHz, channel-hops 1..13, dwell ~110 ms.
- Identity dedup (seen-table), emits CAPTURE on a new identity, periodic
  SUMMARY, always-on HEARTBEAT — all `fh_protocol` frames.
- `src_id = FH_SRC_XS3_WIFI24 (2)`, band nibble 0x2.

## How it talks to the hub (the port)
SPI SLAVE. The hub (master) clocks transactions; this board stages its next
queued frame into the slave buffer and raises **IRQ** to say "come clock me."
Fixed-size 261 B transactions (matches the hub). Inbound PING → PONG reply.
See `main/fh_xs3_link.c` and `WIRING.md`.

## Build / flash (your hardware only)
```
idf.py set-target esp32s3 && idf.py build
idf.py -p <port> flash monitor      # USB-C / USB-Serial-JTAG
```

## Verify first (bench facts — see CONTINUITY.md)
1. Bus A is quad **SPI Slave HD** (`spi_slave_hd`, segment mode). Bring up
   1-bit HD first, then QIO — the QIO dummy-cycle alignment (Espressif #9823) is
   the #1 bench item (link.c L2). See `../remix-BUS-A-QUAD-HD-DECISION.md`.
2. CS/IRQ on GPIO43/44 are the S3 UART0 console pins — verify no boot glitch.
3. 2.4 GHz channel list vs your regulatory domain.

## Files
`main/fh_xs3_main.c` (capture, unchanged) · `fh_xs3_link.{h,c}` (SPI transport,
new) · `fh_xs3_pins.h` · `components/fh_protocol/` (byte-identical) · CMake ×2 ·
`sdkconfig.defaults` · `WIRING.md` · `CONTINUITY.md`.
