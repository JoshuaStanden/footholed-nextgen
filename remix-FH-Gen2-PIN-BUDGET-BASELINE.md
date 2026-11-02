# FH-Gen2 — ESP32-P4-WIFI6-DB Hub Pin Budget (LOCKED BASELINE)

> **Status: LOCKED design baseline, 2026-10-25.** Verified against the real
> Waveshare ESP32-P4-WIFI6-DB pinout (full GPIO function table + the 2x20 header
> silkscreen) and against Espressif's ESP32-P4 GPIO-matrix / QSPI documentation.
> This supersedes the "1 spare" figure in the original
> `ESP32-P4-WIFI6-DB_GPIO_Pin_Budget.xlsx` (that count was pessimistic — see
> Spares below). Every assigned GPIO is header-exposed, free, non-strapping, and
> electrically routable. **Not yet bench-confirmed on hardware** — pin logic is
> proven by reading only.

---

## Architecture this budget serves

```
   Waveshare ESP32-S3 Touch-LCD 3.5  = HEAD (face: owns its own LCD + touch)
   XIAO ESP32-S3                     = 2.4 GHz Wi-Fi sniffer peer
   XIAO ESP32-C5                     = 5 GHz Wi-Fi sniffer peer
        \        |        /
         \       |       /     BUS A: ONE shared quad-SPI bus
          \      |      /      (shared CLK + DQ0..3, per-peer CS + per-peer IRQ,
           v     v     v        time-sliced — one peer talks at a time)
            ESP32-P4  = HUB / bus master (aggregator, logger, time/GPS)
                 |
                 |   BUS B: ONE shared conventional SPI bus
                 v   (shared SCLK/MOSI/MISO + one CS per blade)
            BLADES — one-radio boards (CC1101 / nRF24 / PN532 / SX1262 / LoRa ...)
                     attach directly to the P4
```

- **Bus A** = three ESP32 peers on one shared four-bit (quad) SPI bus.
- **Bus B** = up to six blades on one shared conventional (1-bit) SPI bus.
- Sensor Stick: unchanged, not on either bus here.
- I2C-as-blade-bus is **dead** (too slow for OOK capture / radio streaming).

---

## Bus A — shared quad-SPI to the three ESP32 peers (8 + 3 = 11 GPIO)

| Signal | Hub GPIO | Direction | Notes |
|---|---|---|---|
| QSPI_CLK | GPIO2 | Output | shared clock |
| QSPI_DQ0 | GPIO3 | Bidir | shared data 0 |
| QSPI_DQ1 | GPIO4 | Bidir | shared data 1 |
| QSPI_DQ2 | GPIO5 | Bidir | shared data 2 |
| QSPI_DQ3 | GPIO20 | Bidir | shared data 3 |
| ESP32_1_CS | GPIO21 | Output | peer #1 chip-select |
| ESP32_2_CS | GPIO22 | Output | peer #2 chip-select |
| ESP32_3_CS | GPIO23 | Output | peer #3 chip-select |
| ESP32_1_IRQ | GPIO28 | Input | peer #1 interrupt |
| ESP32_2_IRQ | GPIO29 | Input | peer #2 interrupt |
| ESP32_3_IRQ | GPIO30 | Input | peer #3 interrupt |

Peer ↔ CS/IRQ assignment is now PINNED by `fh_hub_pins.h` + `fh_hub_link.c`
(`busA_init`): peer #1 = **Head** (CS GPIO21 / IRQ GPIO28, wire src_id
FH_SRC_HEAD=4, liveness-only), peer #2 = **XIAO-S3 2.4 GHz** (CS22 / IRQ29,
src_id 2), peer #3 = **XIAO-C5 5 GHz** (CS23 / IRQ30, src_id 1). (Was left
"assign at wiring time" in an earlier draft; the firmware has since fixed it.)

## Bus B — shared conventional SPI to the blades (3 + 6 = 9 GPIO)

| Signal | Hub GPIO | Direction | Notes |
|---|---|---|---|
| SPI_SCLK | GPIO31 | Output | shared clock |
| SPI_MOSI | GPIO32 | Output | shared data hub→blade |
| SPI_MISO | GPIO33 | Input | shared data blade→hub |
| SPI_DEV1_CS | GPIO46 | Output | blade #1 chip-select |
| SPI_DEV2_CS | GPIO47 | Output | blade #2 chip-select |
| SPI_DEV3_CS | GPIO48 | Output | blade #3 chip-select |
| SPI_DEV4_CS | GPIO49 | Output | blade #4 chip-select |
| SPI_DEV5_CS | GPIO50 | Output | blade #5 chip-select |
| SPI_DEV6_CS | GPIO51 | Output | blade #6 chip-select |

Blade radio ↔ CS assignment is now PINNED in `fh_hub_pins.h` (base blades first):
slot0/CS1(GPIO46)=GPS, slot1/CS2(GPIO47)=LoRa(SX1262), slot2/CS3(GPIO48)=CC1101,
slot3/CS4(GPIO49)=nRF24, slot4/CS5(GPIO50)=PN532 (PARKED 2026-11-01 — slot
reserved, see remix-PARKED-CARD-READER-RADIOS.md), slot5/CS6(GPIO51)=spare. Each
blade is a (pico+radio). Wiring may still move a blade (update the define + the
CS wire) until gen-2 auto-discovery makes it dynamic.

**Total assigned: 20 GPIO.**

---

## Spares (CORRECTED — the board exposes 27 header GPIO, not 21)

27 header pins − 20 assigned = **7 unused header GPIO:**

| GPIO | Caveat |
|---|---|
| GPIO7 | Shared **I2C SDA** (ES8311 codec / touch / camera SCCB on this board — but the head owns LCD+touch, so free for hub/blade I2C here) |
| GPIO8 | Shared **I2C SCL** (same) |
| GPIO24 | USB-OTG **D−** — free only if USB-OTG host/device unused |
| GPIO25 | USB-OTG **D+** — free only if USB-OTG host/device unused |
| GPIO26 | Truly free |
| GPIO27 | Truly free |
| GPIO52 | Truly free |

So: **3 unconditional spares (26, 27, 52)**, plus I2C (7/8) and USB (24/25)
available if their alt-functions are unused. Recommend reserving **GPIO7/8 as the
hub's general I2C** (RTC / fuel gauge / future sensor) and keeping 26/27/52 as raw GPIO spares.

---

## Hardware facts that make this valid (verified 2026-10-25)

1. **27 free header GPIO.** Waveshare doc: "2 x 20 pin headers onboard, exposing
   27 remaining programmable GPIOs." Header silkscreen confirms the set:
   2,3,4,5,7,8,20,21,22,23,24,25,26,27,28,29,30,31,32,33,46,47,48,49,50,51,52.
2. **Every assigned pin is in the clean expansion set** — none collide with:
   - onboard **ESP32-C5** SDIO (GPIO14–19), wake (GPIO6), enable (GPIO54)
   - **I2S audio** codec ES8311 (GPIO9–13)
   - **I2C** shared bus (GPIO7/8) — left as hub spare
   - **SD/TF card** SDMMC (GPIO39–45)
   - **UART0 console** CH343P (GPIO37/38)
   - **USB-OTG** (GPIO24/25) — left as spare
   - **strapping pins** (GPIO34/35/36) and **32K XTAL** (GPIO0/1)
3. **Onboard ESP32-C5 is a bonus, not a conflict.** It gives the P4 its OWN
   dual-band Wi-Fi 6 + BLE over SDIO on dedicated pins (zero header cost). It is
   SEPARATE from the external XIAO-C5 sniffer peer on Bus A. Two distinct C5s:
   onboard = hub connectivity; XIAO = 5 GHz capture.
4. **Quad-SPI on GPIO2–5+20 is legal via the GPIO matrix.** ESP32-P4 routes any
   peripheral to any GPIO (IDF v6.1 GPIO doc). These are NOT the dedicated SPI
   IOMUX pins, so the bus runs through the GPIO matrix.
   - **Max clock: ~26–40 MHz** over the matrix (26 full-duplex, up to 40
     half-duplex/quad) vs 80 MHz only on dedicated IOMUX pins.
   - Source: Espressif forum "ESP32-P4: max QSPI frequency through GPIO Matrix"
     → "80 MHz (IOMUX pins) and 26 MHz (GPIO matrix pins)."
   - **Sufficient:** FootHoled buses carry DISTILLED EVENTS, not raw capture
     firehoses. Even 26 MHz ≈ 3 MB/s — ample for the planned peer/blade traffic.
     The dedicated 80 MHz pins are largely consumed by SDIO/SD/C5 anyway and are
     not on the free headers; the matrix route is the only route and it is enough.

---

## Still open (bench / next-step, NOT blocking the baseline)

- **Peer ↔ CS/IRQ** and **blade ↔ CS** concrete assignments (assign at wiring).
- **Bench-confirm** quad mode actually enumerates at target clock on real P4
  silicon (IDF 6.1 SPI master, half-duplex quad).
- **ESP32 slave side:** each peer must run an SPI *slave* that speaks fh_protocol
  framing over the quad bus — the slave-side quad support/pinout per peer board
  (XIAO-S3, XIAO-C5, Waveshare-S3) is its own verification.
- **Pull-ups / idle levels** on shared DQ and CS lines for a multi-drop bus.

_Verified by reading only — no compile, no flash, no hardware. User builds and
confirms on real hardware (ESP-IDF 6.1)._
