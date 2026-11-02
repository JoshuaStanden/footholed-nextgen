# remix-FH-XIAO-C5-firmware — 5 GHz Wi-Fi sniffer (SPI slave)

> Seeed **XIAO ESP32-C5** (new silicon). Pure 5 GHz Wi-Fi promiscuous sniffer.
> In REMIX it is a **SPI SLAVE** on the P4 hub's Bus A (was a UART node in gen1).
> The **capture logic is unchanged** from the proven gen1 firmware — only the
> transport moved (UART → SPI slave + IRQ). It is the **twin of the XIAO-S3**,
> differing ONLY by band. Written by reading only; NOT compiled/flashed here.
> Target: **esp32c5**, ESP-IDF 6.1 (new-silicon 5 GHz API needs 6.1).

## What it does
- esp_wifi promiscuous on **5 GHz** (`WIFI_BAND_MODE_5G_ONLY`), channel set
  `{36,40,44,48,149,153,157,161}` (non-DFS), dwell ~110 ms.
- Identity dedup, CAPTURE / SUMMARY / HEARTBEAT `fh_protocol` frames.
- `src_id = FH_SRC_C5_WIFI5 (1)`, band nibble 0x5.

## How it talks to the hub (the port)
SPI SLAVE, same as the S3: stage a frame, raise **IRQ**, hub clocks it; 261 B
fixed transactions; inbound PING → PONG. See `main/fh_c5_link.c` and `WIRING.md`.

## Build / flash (your hardware only)
```
idf.py set-target esp32c5 && idf.py build
idf.py -p <port> flash monitor      # USB-C / USB-Serial-JTAG
```

## Verify first (bench facts — see CONTINUITY.md)
1. **5 GHz on new silicon** — confirm `esp_wifi_set_band_mode` + 5 GHz
   promiscuous RX work on your IDF 6.1; start 20 MHz, verify HE (11ax) RX before
   widening. **Predicted first failure.**
2. Bus A is quad **SPI Slave HD** (`spi_slave_hd`, segment mode). Bring up
   1-bit HD first, then QIO — dummy-cycle alignment (Espressif #9823), link.c L2.
3. CS/IRQ on GPIO12/11 (not UART0 pins — no console collision like the S3).

## Files
`main/fh_c5_main.c` (capture + 5 GHz init) · `fh_c5_link.{h,c}` (SPI transport) ·
`fh_c5_pins.h` · `components/fh_protocol/` (byte-identical) · CMake ×2 ·
`sdkconfig.defaults` · `WIRING.md` · `CONTINUITY.md`.
