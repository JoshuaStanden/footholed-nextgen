# remix-FH-XIAO-C5-firmware — FUTURE (planning)

Forward-looking notes for the XIAO-C5 5 GHz Wi-Fi sniffer peer (new silicon).

## 1. 5 GHz on new silicon — the gating bench item
This is the predicted first failure. Confirm on YOUR ESP-IDF 6.1 that
`esp_wifi_set_band_mode(WIFI_BAND_MODE_5G_ONLY)` exists and 5 GHz promiscuous RX
actually works. Start at 20 MHz / `WIFI_SECOND_CHAN_NONE`; verify HE (11ax) RX on
a known 5 GHz AP BEFORE widening the channel/bandwidth. If RX is empty, this is a
silicon/IDF-API problem, not a wiring one — isolate it standalone before wiring
the hub link.

## 2. Bring order: S3 first, then C5
Prove the twin XIAO-S3 end-to-end first (mature silicon, same transport). Once a
frame moves on the S3, the C5 adds only the 5 GHz delta on top of proven
transport — so a C5 failure then points squarely at the 5 GHz path, not the SPI
link.

## 3. Bus-A HD bring-up + throughput
Same sequence as the S3: bring up 1-bit SPI Slave HD, prove one
`essl_spi_rddma`/`essl_spi_wrdma` round trip, then widen the hub to QIO and align
dummy cycles (Espressif #9823, `fh_c5_link.c` L2). The old 1-bit seam is
RESOLVED. The Bus-A throughput upgrade is the hub's job (`link.c` R4), not this
peer's.

## 4. DFS channels
The starter set is non-DFS. Adding DFS channels (52–64, 100–140) needs
radar-detect / channel-availability-check handling that is not implemented —
a deliberate later feature, gated by regulatory need.

## 5. Not applicable
No pico-sdk port applies here — that is a Pico-blade concern. This is an ESP32
`spi_slave_hd` (SPI Slave HD) at MODE 0.
