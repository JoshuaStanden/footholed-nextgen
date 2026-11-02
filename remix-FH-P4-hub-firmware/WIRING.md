# remix-FH-P4-hub — WIRING

> Physical hookup for the **Waveshare ESP32-P4-WIFI6-DB** hub. Every GPIO here
> is from the **verified pin budget** (`../remix-FH-Gen2-PIN-BUDGET-BASELINE.md`),
> checked against the real board's GPIO table + the 2×20 header silkscreen.
> **STILL verify each pin against YOUR physical board at bench** — a boot/
> continuity test per pin before you trust it. Nothing here was flashed on the
> authoring machine.
>
> Legend:  `->` hub drives it (output from P4)   `<-` into the hub (input)
>          `<>` bidirectional (shared SPI data)

---

## The two buses at a glance

```
                         ESP32-P4-WIFI6-DB  (HUB / bus master)
                         ┌───────────────────────────────────┐
  BUS A (SPI2, QUAD, IRQ-driven)                               (BUS B, SPI3, conventional)
  shared CLK + DQ0..3, per-peer CS + IRQ                       shared SCLK/MOSI/MISO, per-blade CS
        │                                                              │
   GPIO2  CLK  ->                                                 GPIO31 SCLK ->
   GPIO3  DQ0  <>        ┌── CS  GPIO21 -> Waveshare-S3 HEAD       GPIO32 MOSI ->
   GPIO4  DQ1  <>        │   IRQ GPIO28 <-                         GPIO33 MISO <-
   GPIO5  DQ2  <>        ├── CS  GPIO22 -> XIAO-S3 (2.4 GHz)       GPIO46 CS1 -> blade 1
   GPIO20 DQ3  <>        │   IRQ GPIO29 <-                         GPIO47 CS2 -> blade 2
                         └── CS  GPIO23 -> XIAO-C5 (5 GHz)         GPIO48 CS3 -> blade 3
                             IRQ GPIO30 <-                         GPIO49 CS4 -> blade 4
                                                                   GPIO50 CS5 -> blade 5
                                                                   GPIO51 CS6 -> blade 6
```

---

## BUS A — quad-SPI to the 3 ESP32 peers  (SPI3_HOST)

> Host swap 2026-10-30 (bench): the P4's SPI3 has only **3** hardware CS lines
> and SPI2 has **6**. Bus A needs 3, Bus B needs 6, so Bus A = SPI3 and Bus B =
> SPI2. Pins are unchanged (both buses use the GPIO matrix).

Shared bus lines (fan out to ALL three peers):

| P4 GPIO | Signal | Dir | Goes to |
|---|---|---|---|
| GPIO2  | QSPI_CLK  | -> | CLK on head + XIAO-S3 + XIAO-C5 |
| GPIO3  | QSPI_DQ0  | <> | DQ0 on all three |
| GPIO4  | QSPI_DQ1  | <> | DQ1 on all three |
| GPIO5  | QSPI_DQ2  | <> | DQ2 on all three |
| GPIO20 | QSPI_DQ3  | <> | DQ3 on all three |

Per-peer select + interrupt (one each):

| Peer | CS (P4 ->) | IRQ (P4 <-) | Role | Wire src_id |
|---|---|---|---|---|
| Waveshare-S3 HEAD | GPIO21 | GPIO28 | display/face (renders hub feed) | none |
| XIAO-S3 | GPIO22 | GPIO29 | 2.4 GHz Wi-Fi sniffer | 2 (FH_SRC_XS3_WIFI24) |
| XIAO-C5 | GPIO23 | GPIO30 | 5 GHz Wi-Fi sniffer | 1 (FH_SRC_C5_WIFI5) |

- **IRQ idle LOW, asserted HIGH** ("I have a frame"). Hub configures the IRQ
  GPIOs input + pull-DOWN, rising-edge interrupt.
- **Multidrop data caution (SLAVE-side rule):** DQ0..3 are shared. Each peer
  MUST tri-state its DQ outputs unless its own CS is low, or two peers will
  fight the bus. This is a requirement on the peer (slave) firmware, not the hub.
- Clock: start at 10 MHz; GPIO-matrix caps ~26–40 MHz. Short wiring between
  boards matters for signal integrity as you raise it.

---

## BUS B — conventional SPI to the blades  (SPI2_HOST)

Shared bus lines (fan out to ALL blades):

| P4 GPIO | Signal | Dir | Goes to |
|---|---|---|---|
| GPIO31 | SCLK | -> | SCLK on every blade |
| GPIO32 | MOSI | -> | MOSI (hub -> blade) on every blade |
| GPIO33 | MISO | <- | MISO (blade -> hub) on every blade |

Per-blade select (one each; assign radios at wiring time):

| Slot | CS (P4 ->) | Pass-1 default assignment |
|---|---|---|
| 1 | GPIO46 | **GPS blade** (pico+NEO-6M, polled; `FH_BLADE_SLOT_GPS = 0` → CS1) |
| 2 | GPIO47 | **LoRa blade** (pico+SX1262; `FH_BLADE_SLOT_SX1262 = 1` → CS2) |
| 3 | GPIO48 | **CC1101 blade** (pico+CC1101 sub-GHz; `..._CC1101 = 2` → CS3) |
| 4 | GPIO49 | **nRF24 blade** (pico+nRF24 2.4 GHz; `..._NRF24 = 3` → CS4) |
| 5 | GPIO50 | **PN532 blade — PARKED** (slot reserved, no firmware; `..._PN532 = 4` → CS5; see `../remix-PARKED-CARD-READER-RADIOS.md`) |
| 6 | GPIO51 | (spare blade) |

> REMIX = one radio per (pico+radio) blade (not the gen1 one-Pico-four-radios).
> BASE BLADES FIRST: GPS and LoRa are on essentially every device, so they take
> slots 0 and 1; the fired recon radios follow.
> NOTE on slot numbering: `FH_BLADE_SLOT_*` are **0-based array indices** in
> firmware; the CS pins are labelled CS1..CS6 (1-based) on the silkscreen. So
> firmware slot 0 = CS1 (GPIO46) = GPS; slot 1 = CS2 (GPIO47) = LoRa. Keep this
> mapping consistent when you wire — it is the one easy off-by-one here.
- Blades are **fired one-at-a-time** (BUSY contract); GPS is **polled** on a
  schedule (1 Hz / 5 Hz from config). Clock 8 MHz (radios are low-rate).

---

## microSD (the log sink) — DEDICATED, not on either bus

The P4-WIFI6-DB TF slot is a dedicated **SDMMC 4-bit** interface. These pins are
fixed by the board and owned by the SDMMC driver — **do not** route a bus here.

| P4 GPIO | SD signal |
|---|---|
| GPIO39 | SD_D0 |
| GPIO40 | SD_D1 |
| GPIO41 | SD_D2 |
| GPIO42 | SD_D3 |
| GPIO43 | SD_CLK |
| GPIO44 | SD_CMD |
| GPIO45 | SD_VDD_EN (low = enable) |

Insert a FAT-formatted microSD. The logger is the SINGLE writer under `/sdcard`.

---

## Console + BLE — no header pins

- **Console / Medusa:** the **USB-C** port goes to an onboard **CH343 USB-UART**
  on UART0 (TX GPIO37 / RX GPIO38), **921600 baud**. It is NOT the P4's native
  USB-Serial-JTAG. One cable does flash + monitor + the Medusa binary bridge.
  The CH343's DTR/RTS drive the auto-reset (EN/BOOT): a host that opens the
  port with them asserted reboots the hub (Medusa now opens with them off).
- **BLE sniff:** the P4's **onboard ESP32-C6** co-processor (SDIO slot 1,
  GPIO14–19 + reset GPIO54, dedicated, zero header cost). Hub-local, off-bus.
  Live. It shares the SDMMC controller with the TF card (slot 0).

---

## Spare header GPIO (unused in pass 1)

| GPIO | Note |
|---|---|
| GPIO7 / GPIO8 | shared **I2C** SDA/SCL — free on the hub (head owns LCD+touch); reserve for a hub-local RTC / fuel-gauge if ever added |
| GPIO24 / GPIO25 | USB-OTG D−/D+ — free only if USB-OTG unused |
| GPIO26 / GPIO27 / GPIO52 | unconditionally free |

---

## Verify-at-bench checklist (per pin, before trusting)
1. Confirm each GPIO above is the one on YOUR board's 2×20 header silkscreen.
2. Common ground between the P4 and every peer/blade — a solid ground matters
   more than the exact clock; the frame CRC turns noise into dropped frames,
   not corruption.
3. Bring up ONE peer on Bus A first (CS + IRQ + the shared lines) and prove a
   frame moves before adding the second and third.
4. Bring up the GPS blade on Bus B alone and confirm the scheduled poll returns
   a 0x02 fix before adding fired radios.
5. SDMMC: if the 4-bit default slot is not the TF slot, hard-set
   `slot.clk/cmd/d0..d3 = 43/44/39/40/41/42` (see `main/fh_hub_board.c` D1).
