# remix-FH-WS-head-firmware — WIRING

> Waveshare **ESP32-S3-Touch-LCD-3.5 (NON-B)** = the FootHoled **HEAD** (face).
> Display/touch/power use the **PROVEN original-FootHoled board adapter**
> (`fh_ws_board.cpp`), bench-working on this exact board. REMIX adds a Bus-A
> **SPI Slave HD** peer link to the P4 hub.

## Confirmed hardware (Waveshare wiki + original FootHoled josh-dev)
- MCU: **ESP32-S3R8** — 8 MB **OCTAL** PSRAM, 16 MB flash. (NOT quad — see below.)
- Display: **ST7796** over **standard SPI** (not QSPI), 320×480 IPS, BGR+inverted.
- Touch: **FT6336** over **I2C** (addr 0x38, polled — INT not wired).
- LCD reset + touch reset: on a **TCA9554 I2C expander**, NOT ESP32 GPIOs.
- Also on the shared I2C0: QMI8658 IMU, PCF85063 RTC, AXP2101 PMIC, ES8311 codec.

## Display / touch / power pins — FIXED by the board (owned by fh_ws_board.cpp)
| Signal | GPIO | Notes |
|---|---|---|
| LCD MOSI | GPIO1 | SPI2, data to panel |
| LCD SCLK | GPIO5 | SPI2 |
| LCD DC | GPIO3 | data/command |
| LCD CS | NC | not a dedicated GPIO |
| LCD RST | NC | **on TCA9554 pin 1** |
| LCD backlight | GPIO6 | **LEDC PWM** (not a plain GPIO) |
| I2C SDA | GPIO8 | shared I2C0 (header pin 28) |
| I2C SCL | GPIO7 | shared I2C0 (header pin 26) |
| Touch INT | NC | polled |
| Touch RST | NC | on TCA9554 |

These are defined inside `fh_ws_board.cpp` (one source of truth, the proven
values) — do not duplicate them in `fh_ws_pins.h`.

## Bus A — SPI Slave HD link to the P4 hub (REMIX, on SPI3)
The LCD owns SPI2, so the Bus-A HD slave is on **SPI3**. GPIOs are PLACEHOLDERS
on the 2.54 mm expansion header — VERIFY each is free (not a display/I2C pin,
not a strapping pin 0/3/45/46).

| Head GPIO | Signal | Hub side |
|---|---|---|
| GP12 | SCK  (hub drives) | FH_BUSA_CLK (GP2) |
| GP11 | MOSI/DQ0 | FH_BUSA_DQ0 (GP3) |
| GP13 | MISO/DQ1 | FH_BUSA_DQ1 (GP4) |
| GP14 | DQ2 (QUADWP) | FH_BUSA_DQ2 (GP5) |
| GP21 | DQ3 (QUADHD) | FH_BUSA_DQ3 (GP20) |
| GP10 | CS | FH_BUSA_CS_HEAD (GP21) |
| GP9  | IRQ "frame staged" | FH_BUSA_IRQ_HEAD (GP28) |

HD params FROZEN bus-wide: mode 0, command/address/dummy = 8 bits, 261 B
segments. Bring up **1-bit HD first**, then QIO (dummy-cycle risk, Espressif
#9823 — see `../remix-BUS-A-QUAD-HD-DECISION.md`).

## PSRAM — OCTAL, do NOT set QUAD
The ESP32-S3**R8** is **octal** PSRAM ("anything larger than 4 MB is octal").
`sdkconfig.defaults` keeps `CONFIG_SPIRAM_MODE_OCT=y`. QUAD mode never inits the
R8's PSRAM, init fails at boot, draw buffers can't allocate, display dies. The
"8 MB PSRAM" marketing text misled an earlier review into QUAD — do not repeat it.

## Bench bring-up order
1. **Display alone** (no hub): the TCA9554 reset pulse + ST7796 init + LVGL draw.
   Expect "FootHoled HEAD / waiting for hub...". Blank screen → check the TCA9554
   reset pulse ran and PSRAM is OCT (the two historical blank-screen causes).
2. **Touch**: tap; LVGL registers the press (and fires the upstream sentinel).
   Touch coords may need swap/mirror per rotation — see DISPLAY_NOTES open item.
3. **Bus-A link** to the hub (SPI3): 1-bit HD first → PING→PONG → Medusa node-4
   green, then widen to QIO.
