# remix-fh-pico-sdk-gps — WIRING

> Raspberry Pi **Pico (RP2040)** + **NEO-6M GPS**, native Pico C/C++ SDK blade.
> A SPI **slave** on the P4 hub's **Bus B**. GPn = silkscreen GPIO. These pins
> are the **bench-confirmed, working** values (MISO/MOSI reconciled 2026-11-01)
> — the blade is PROVEN on hardware. **All 3.3V.**
>
> The five-doc set: **WIRING** (this file) · **README** (people) · **AGENTS**
> (AI/LLM) · **CONTINUITY** (resume) · **FUTURE** (planning). The hard-won RP2040
> lessons live in `PICO_LESSONS.md` (kept under its own name; repo-wide refs).

## SPI slave link to the hub (Bus B) — spi0, hub is MASTER
| Signal | Pico GPn | Physical pin | Direction | Note |
|---|---|---|---|---|
| SPI SCK  | GP18 | 24 | hub → blade | spi0 SCK, 8 MHz |
| SPI MOSI | GP16 | 21 | hub → blade | spi0 RX (slave) |
| SPI MISO | GP19 | 25 | blade → hub | spi0 TX (slave) |
| SPI CS   | GP21 | 27 | hub → blade | hardware CSn (GP21 valid) |

- **MOSI = GP16 / MISO = GP19** are the correct, bench-confirmed values (after
  the MISO/MOSI reconcile). On the RP2040 spi0, GP16 is RX and GP19 is TX, so in
  slave mode GP16 receives the hub's MOSI and GP19 drives MISO back to the hub.
- Valid spi0 CSn pins are ONLY GP1/GP5/GP17/GP21 — CS is GP21.
- Hub side: GPS = slot 0 = CS1 = GPIO46; shared SCLK/MOSI/MISO = GPIO31/32/33.

## GPS module (NEO-6M) — uart1
| Signal | Pico GPn | Physical pin | Direction | Note |
|---|---|---|---|---|
| GPS TX (blade → GPS RX) | GP4 | 6 | out | uart1 TX, UBX 5 Hz config |
| GPS RX (GPS TX → blade) | GP5 | 7 | in  | uart1 RX, NMEA |

## Other
| Signal | Pico GPn | Note |
|---|---|---|
| LED (on-board) | GP25 | solid 1 s per SPI transaction; 5 fast blinks at boot |
| IRQ OUT (optional) | GP6 | HIGH = fresh fix; unused by the current hub |
| DBG TX (optional) | GP0 | uart0 115200, OFF by default (`FH_DEBUG_UART=1`) |
| DBG RX (optional) | GP1 | uart0 115200, OFF by default |

**Ground matters:** common GND between the blade, the hub, and the NEO-6M.

## SPI mode — MODE 1 (proven)
This blade runs the spi0 slave at **MODE 1 (CPOL=0, CPHA=1)** —
`spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST)`. On the
RP2040 a MODE 0 SSP slave sends only one byte per CS assertion (byte 0, then
0xff) — MODE 1 is why this blade streams the full 261-byte frame. The hub drives
Bus B in MODE 1 to match. See `PICO_LESSONS.md` §1 and `../AGENTS.md` §1.

## Bench bring-up (PROVEN path — this is the reference blade)
1. Flash the `.uf2` (BOOTSEL → RPI-RP2 drive → PowerShell `Copy-Item` + eject;
   a bare `cp` does NOT flash). **5 fast LED blinks at boot = new firmware
   running.**
2. The CFG-RATE verdict blinks on the LED: 3 quick blips = module is 5 Hz-capable
   (ACK); 1 slow blink = rate-locked clone, stays 1 Hz (still fine, NMEA parses).
3. Hub side: the GPS poll task clocks the blade on `cfg.gps_rate` cadence; the
   blade answers with a `FH_TYPE_GPS_FIX` (0x02) frame. The hub's `fh_ui` status
   line shows `PICO=UP` and `RTC=ok` once a fix with a plausible epoch locks the
   clock.

## Indoor bench (no sky view)
Build with `-DFH_GPS_SIM=1` to inject a hardcoded valid fix (51.5 N, −0.12 W,
8 sats) through the normal staging path — proves encode → SPI → decode → RTC
without a GPS lock. **Never ship a field rig with `FH_GPS_SIM=1`.**

## Verify-at-bench
- Confirm each GP against YOUR Pico silkscreen. Common ground to the hub + GPS.
- Full build/flash recipe is in `README.md` and `PICO_LESSONS.md` §4–5.
