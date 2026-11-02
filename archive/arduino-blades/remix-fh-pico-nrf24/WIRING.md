# remix-fh-pico-nrf24 — WIRING

> Pico (RP2040) + nRF24L01. **TWO SPI blocks:** SPI0 master to the radio chip,
> SPI1 slave to the P4 hub (Bus B). GPn = silkscreen GPIO — VERIFY on YOUR
> board. nRF24 VCC is **3.3V ONLY** (Pico 3V3 pin 36). A **10µF cap** across
> nRF24 VCC/GND is strongly recommended (brown-out resets, esp. +PA+LNA).
>
> The five-doc set: **WIRING** (this file) · **README** (people) · **AGENTS**
> (AI/LLM) · **CONTINUITY** (resume) · **FUTURE** (planning, incl. pico-sdk port).

## Radio bus — nRF24 on SPI0 (master) — carried from gen1
| nRF24 | Pico GPIO | Notes |
|---|---|---|
| VCC | 3V3 (pin 36) | 3.3V ONLY — 5V kills it; add 10µF cap |
| GND | any GND | common ground |
| SCK | GP18 | SPI0 SCK |
| MOSI | GP19 | SPI0 TX |
| MISO | GP16 | SPI0 RX |
| CSN | GP17 | SPI chip select |
| CE | GP20 | chip enable (RX/TX strobe) |

## Hub link — SPI1 slave to the P4 Bus B
| Pico GPIO | Signal | Dir | Hub side (P4) |
|---|---|---|---|
| GP10 | SCK  (hub drives) | in  | FH_BUSB_SCLK = GPIO31 |
| GP12 | MOSI (hub → blade)| in  | FH_BUSB_MOSI = GPIO32 |
| GP11 | MISO (blade → hub)| out | FH_BUSB_MISO = GPIO33 |
| GP13 | CS   (hub selects)| in  | nRF24 blade = slot 3 = FH_BUSB_CS4 = GPIO49 |
| GP8  | IRQ "result ready" (OPTIONAL) | out | a hub Bus-B spare input, or unwired |

- SPI0 (radio) and SPI1 (hub) are independent hardware — no shared pin. Verify
  GP10–13 + GP8 don't clash with GP16–20 and are a valid spi1 group.
- Common **ground** between the Pico and the hub is mandatory.
- **SLOT NOTE:** `FH_BLADE_SLOT_NRF24 = 3` (0-based firmware index) = CS4 =
  GPIO49 on the hub. (GPS=slot0, LoRa=slot1, CC1101=slot2 precede it.)

## Power / RF
- Pico from the hub rail or its own USB. All GPIO 3.3V — no level shifting.
- The **10µF cap** at the module is the single most common fix for a false
  `radio=ABSENT`/brown-out on +PA+LNA modules.

## SPI mode — NON-NEGOTIABLE
Hub-link SPI slave is **MODE 1 (CPOL=0, CPHA=1)**. On the RP2040 a MODE 0 SSP
slave transmits only ONE byte per CS assertion (byte 0, then 0xff underrun) — no
pump/preload fix helps. The hub drives Bus B in MODE 1 and the bench-proven GPS
blade is MODE 1. See `AGENTS.md` / `../AGENTS.md` §1. Do NOT set SPI_MODE0.

## nRF24 reality — NO true RSSI
The nRF24 has only a 1-bit carrier-detect (RPD), not a dBm. The scan reports
WHICH channel showed a carrier (`active_chan`, 0xFF = none); `resp.rssi` stays 0
by design. Never invent a dBm the chip cannot measure. FRAMEWORK ONLY — the scan
is a minimal presence probe to exercise the REQ/RESP path, not a capture module.

## Bench bring-up order
1. **Boot + radio present.** Flash; USB serial @115200. Expect
   `remix-fh-pico-nrf24 v1 (SPI) boot  radio=ok`. `radio=ABSENT` → the classic
   false-absent is the SPI0 pin binding or a missing decoupling cap. Check
   GP18/19/16 + CSN GP17 + CE GP20, the 3V3 rail, and add the **10µF cap**.
   `radio.begin(&SPI)` must get the SPI object whose pins were set first.
2. **Chip really answering?** Type `s` → status (channel + variant).
   `isChipConnected()` is the strong check; a +PA+LNA module reports `+PA+LNA`,
   a plain one `standard`.
3. **Hub link.** Wire SPI1 to the hub Bus B. From the hub/Medusa:
   `FH_OD_PING_REQ → FH_OD_PING_RESP` (NRF24 radios_ready bit); then
   `FH_OD_NRF24_REQ` (chan_lo=0, chan_hi=80, dwell_ms=2) → `FH_OD_NRF24_RESP`.
   With a 2.4 GHz source on a channel in range, `active_chan` = that channel;
   else `0xFF` / `NO_RESULT`. SNIFF (mode 1) dwells one channel and streams
   payloads as `FH_OD_DUMP_IND` chunks before the summary RESP.

## Failure decision tree
| Symptom | Likely cause | Action |
|---|---|---|
| radio=ABSENT | SPI0 binding / no cap / wiring | step 1; add 10µF cap |
| scan always 0xFF | RPD threshold (~−64 dBm) / no source | bring a 2.4 GHz source closer |
| hub gets nothing | SPI1 wiring / CS / SPI mode / SPISlave1 | verify SPI1 pins, MODE 1, common ground |
| hub gets byte-0-then-0xff | SPI MODE 0 (should be MODE 1) or SPISlave1 can't stream | set MODE 1; if still failing see FUTURE (pico-sdk port) |
| compile error testRPD | RF24 version lacks it | use testCarrier() fallback |
| compile error SPISlave1 | arduino-pico version differs | confirm onDataRecv/onDataSent on your core |

## Verify-at-bench
- Confirm every GPIO above against YOUR Pico silkscreen before soldering.
- Common ground between Pico, hub, and radio module; the 10µF cap at the nRF24.
- Prove SPI0 (radio) first, then the SPI1 hub link.
