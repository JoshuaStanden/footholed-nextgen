# remix-fh-pico-sdk-sx1262 — WIRING

> Pico (RP2040) + Seeed **Wio-SX1262**, native **Raspberry Pi Pico C/C++ SDK**.
> **TWO SPI blocks:** hub link on **spi0** (slave, MODE 1, copied from the
> bench-proven GPS blade), SX1262 radio on **spi1** (master) via RadioLib +
> PicoHal. GPn = silkscreen GPIO — VERIFY on YOUR board. Wio-SX1262 is 3.3V.
>
> Five-doc set: **WIRING** (this) · **README** (people) · **AGENTS** (AI
> agents) · **CONTINUITY** (resume) · **FUTURE** (planning). Plus
> **PICO_LESSONS.md** (verbatim copy of the GPS master).

## SPI split — note it differs from the arduino sx1262 blade
The arduino blade put the radio on SPI0 and the hub on SPI1. This SDK port
**inverts that**: hub link on **spi0** (byte-identical to the proven GPS pump),
radio on **spi1**. Same inversion as the cc1101 SDK port.

## Hub link — spi0 slave to the P4 Bus B (SAME pins as the GPS blade)
| Pico GPIO | Signal | Dir | Hub side (P4) |
|---|---|---|---|
| GP18 | SCK  (hub drives) | in  | FH_BUSB_SCLK |
| GP16 | MOSI (hub → blade)| in  | FH_BUSB_MOSI (spi0 RX on slave) |
| GP19 | MISO (blade → hub)| out | FH_BUSB_MISO (spi0 TX on slave) |
| GP21 | CS   (hub selects)| in  | SX1262 blade slot CS |
| GP2  | IRQ "RX/result ready" (OPTIONAL) | out | a hub Bus-B spare input |

- MOSI=GP16 / MISO=GP19 is the **working** GPS convention — do NOT swap.
- **CS is a plain polled input here, NOT an edge IRQ** (R-IRQBANK): RadioLib's
  PicoHal owns the GPIO bank IRQ on core1, so core0 polls CS instead.
- Common ground between the Pico and the hub is mandatory.

## Radio — Wio-SX1262 on spi1 (master)
Confirmed from the module silkscreen (Wio-SX1262, FCC ID Z4T-WIO-SX1262). Two
7-pad rows flank the 2.2 mm mounting hole; IPEX LoRa antenna connector bottom.

**Left row (top→bottom):** DIO1 · BUSY · RST · NSS · RF-SW · D5 · D6
**Right row (top→bottom):** VIN · GND · 3V3 · MOSI · MISO · SCK · D7

| Module pad | Pico GPIO | Notes |
|---|---|---|
| 3V3 | 3V3 (pin 36) | feed the 3.3V rail here (module is DC-DC) |
| GND | GND | common ground (any Pico GND) |
| SCK | GP10 | spi1 SCK |
| MOSI | GP11 | spi1 TX |
| MISO | GP12 | spi1 RX |
| NSS | GP13 | SPI chip-select |
| RST | GP8 | reset |
| BUSY | GP9 | RadioLib polls |
| DIO1 | GP7 | RadioLib IRQ (PicoHal bank handler, core1) |
| RF-SW | GP6 | antenna/RF switch (`FH_SX_RFSW1` in code) — VERIFY sense |

Pads NOT wired for this blade:
- **VIN** — alternative regulated input; we power via **3V3** instead. Do not
  feed both. (Pick one rail; 3V3 direct is simplest from the Pico 3V3 pin.)
- **D5 / D6 / D7** — the module's spare GPIO pass-through pads (mapped to the
  XIAO host's GPIOs in the kit). Standalone they are unused by the SX1262 — leave
  them unconnected.
- **User LED / User Button** — on-module, not needed for the blade.

**STANDALONE USE (how we're using it):** the module ships in the XIAO
plug-and-play form factor, but it is **separated from its XIAO host and
hand-wired to the Pico** — the XIAO footprint is NOT used, so the XIAO/ESP32S3
GPIO numbers from Seeed's kit examples (NSS=41, DIO1=39, RST=42, BUSY=40, SCK=7,
MOSI=9, MISO=8) are the old host's pins and **do NOT apply here.** Wire each
module pad above to its listed Pico GPIO; the Pico-side GPIOs are OUR choice.
Note Seeed's silkscreen says **RF-SW** (the code calls it `FH_SX_RFSW1` — same
signal). DIO2 is not broken out; DIO3 = TCXO is internal.

- DIO3 = TCXO supply is **internal** to the module (no host pin); RadioLib
  `begin()` tcxoVoltage=1.6 V enables it.
- The two SPI blocks share no pin. Verify GP10–13/7/8/9/6 don't clash with the
  spi0 group (GP16/18/19/21) on your board.

## SPI modes — different per block, both correct
- **Hub link = spi0 slave MODE 1** (CPOL0/CPHA1). RP2040 one-byte-per-CS rule.
- **Radio = spi1 master MODE 0** (CPOL0/CPHA0) — PicoHal's `spiBegin()` sets it.

## Power / RF
- Watch the 3V3 rail under a LoRa TX current spike (+22 dBm PA); a bulk cap near
  the module helps. Keep the IPEX/U.FL **antenna attached before any TX** — TX
  into an open can damage the PA. LoRa TX is **authorized-use-only** (comms to
  YOUR sensor stick), never auto.

## Bench bring-up order
1. **Boot + radio present.** Flash; LED 1 blink (boot), 2 (core1 launched).
   core1 runs `sx_init(915.0)` (set YOUR band) and caches `s_radio_ready`.
2. **Hub link alone.** `FH_OD_PING_REQ → FH_OD_PING_RESP` with the SX1262
   radios_ready bit set, and `FH_TYPE_PING → FH_TYPE_PONG`. Proves the copied
   (now CS-polled) GPS pump carries on this blade.
3. **LoRa TX.** `FH_OD_LORA_TX_REQ` (≤200 B) → `FH_OD_LORA_TX_RESP` status=OK +
   toa_ms. Antenna attached. Confirm the sensor stick receives it.
4. **Listen window.** `FH_OD_LORA_LISTEN_REQ` enable=1 (window_ms=0 continuous)
   → `FH_OD_LORA_LISTEN_RESP`; then send from the stick → an unsolicited
   `FH_OD_LORA_RX_IND` (seq/rssi/snr + data) arrives at the hub.
5. A second REQ while an op runs → `status=BUSY` (correct, one-at-a-time).

## Failure decision tree
| Symptom | Likely cause | Action |
|---|---|---|
| PING_RESP SX1262 bit clear | begin() failed | serial code: -707 TCXO, -2 SPI/DIO wiring (step 1) |
| hub gets nothing on PING | spi0 wiring / CS / MODE | verify spi0 pins, MODE 1, ground (step 2) |
| hub byte-0-then-0xff | spi0 set to MODE 0 | set MODE 1 (PICO_LESSONS) |
| TX returns RADIO_FAIL | RF_SW1 sense / antenna / begin | check RF_SW1 level, antenna, begin() code |
| no RX_IND while listening | DIO1 wiring / getPacketLength not latching | check GP7/DIO1; see R-RXPOLL (may need IRQ-flag gate) |
| pump slips at speed | CS polling can't keep up | see R-IRQBANK fallback (PIO CS realign) |

## Verify-at-bench
- Confirm every GPIO against YOUR Pico + Wio silkscreen before soldering.
- Prove the hub link (spi0) first, then the radio (spi1).
- ISM band is **915 MHz (North America)**, hardcoded in `sx_init()` — committed.
