# remix-FH-XIAO-S3-firmware — FUTURE (planning)

Forward-looking notes for the XIAO-S3 2.4 GHz Wi-Fi sniffer peer.

## 1. Prove a frame end-to-end
The capture path is the proven gen1 logic; what is unproven is the new SPI-slave
transport moving a frame to the hub. Bring this peer up FIRST (mature S3 silicon,
ahead of the new-silicon C5): flash standalone, confirm 2.4 GHz promiscuous RX +
the heartbeat console line, then wire the SPI slave + IRQ to the hub and prove
ONE capture row lands before adding the C5 peer.

## 2. Bus-A HD bring-up (1-bit HD -> QIO)
Bus A is committed to quad **SPI Slave HD** (`../remix-BUS-A-QUAD-HD-DECISION.md`);
the old "quad master vs 1-bit slave" seam is RESOLVED (the S3 DOES have the HD
slave peripheral — `SOC_SPI_SUPPORT_SLAVE_HD_VER2`). Bench sequence: (1) bring up
1-bit HD and prove one Rd_DMA/Wr_DMA round trip with the hub's
`essl_spi_rddma`/`essl_spi_wrdma`; (2) widen the hub's transactions to QIO and
align dummy cycles on both sides (Espressif #9823, `fh_xs3_link.c` L2). The
slave code does not change to go QIO — the master selects the width.

## 3. CS/IRQ pin fallback
If boot-time UART0 activity glitches CS/IRQ on GPIO43/44, move both to
SPI-unused free pins and update the hub side (`FH_BUSA_CS_XS3` / `FH_BUSA_IRQ_XS3`)
to match. Documented so the fallback is ready, not discovered mid-bench.

## 4. Bus-A throughput
A multi-frame drain / two-phase length header for a bursty peer is the HUB's
upgrade (`link.c` R4) — this peer just stages frames and raises IRQ; it does not
change when the hub's drain model improves.

## 5. Not applicable
No pico-sdk port applies here — that is a Pico-blade concern (the RP2040
`SPISlave1` transmit issue). This is an ESP32 `spi_slave_hd` (SPI Slave HD) at
MODE 0, a different driver and a different (correct) mode.
