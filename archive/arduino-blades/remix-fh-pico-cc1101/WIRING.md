# remix-fh-pico-cc1101 — WIRING

> Pico (RP2040) + CC1101. **TWO SPI blocks:** SPI0 master to the radio chip,
> SPI1 slave to the P4 hub (Bus B). GPn = silkscreen GPIO — VERIFY on YOUR
> board. CC1101 VCC is **3.3V ONLY** (Pico 3V3 pin 36, never 5V/VBUS).
>
> The five-doc set for this blade: **WIRING** (this file, how it's wired) ·
> **README** (for people) · **AGENTS** (for AI/LLM agents) · **CONTINUITY**
> (resume-after-a-drop) · **FUTURE** (planning, incl. the pico-sdk port).

## Radio bus — CC1101 on SPI0 (master) — carried from gen1
| CC1101 | Pico GPIO | Notes |
|---|---|---|
| VCC | 3V3 (pin 36) | 3.3V ONLY — 5V destroys it |
| GND | any GND | common ground |
| SCK | GP18 | SPI0 SCK |
| MOSI (SI) | GP19 | SPI0 TX |
| MISO (SO) | GP16 | SPI0 RX |
| CSN | GP17 | chip select |
| GDO0 | GP20 | RX data (capture IRQ) + TX bit-bang |
| GDO2 | GP21 | reserved (unused v0) |

## Hub link — SPI1 slave to the P4 Bus B
| Pico GPIO | Signal | Dir | Hub side (P4) |
|---|---|---|---|
| GP10 | SCK  (hub drives) | in  | FH_BUSB_SCLK = GPIO31 |
| GP12 | MOSI (hub → blade)| in  | FH_BUSB_MOSI = GPIO32 |
| GP11 | MISO (blade → hub)| out | FH_BUSB_MISO = GPIO33 |
| GP13 | CS   (hub selects)| in  | CC1101 blade = slot 2 = FH_BUSB_CS3 = GPIO48 |
| GP8  | IRQ "result ready" (OPTIONAL) | out | a hub Bus-B spare input, or unwired |

- The two SPI blocks are independent hardware — SPI0 (radio) and SPI1 (hub)
  never share a pin. **Verify GP10–13 + GP8 don't clash with GP16–21** and are
  a valid spi1 group on your board.
- Common **ground** between the Pico and the hub is mandatory.
- **SLOT NOTE:** `FH_BLADE_SLOT_CC1101 = 2` (0-based firmware index) = CS3 =
  GPIO48 on the hub. (GPS=slot0/CS1, LoRa=slot1/CS2 are the base pair ahead of
  it.) Keep firmware-index ↔ silkscreen-CS mapping straight when you wire.

## Power / RF
- Pico from the hub rail or its own USB. All GPIO 3.3V — no level shifting.
- CC1101: a stable 3.3V; keep the antenna attached before any TX (replay/send).

## SPI mode — NON-NEGOTIABLE
Hub-link SPI slave is **MODE 1 (CPOL=0, CPHA=1)**. On the RP2040 a MODE 0 SSP
slave transmits only ONE byte per CS assertion (byte 0, then 0xff underrun) — no
pump/preload fix helps. The hub drives Bus B in MODE 1 and the bench-proven GPS
blade is MODE 1. See `AGENTS.md` / `../AGENTS.md` §1. Do NOT set SPI_MODE0.

## Bench bring-up order
1. **Boot + radio present.** Flash; open USB serial @115200. Expect
   `remix-fh-pico-cc1101 v1 (SPI) boot  radio=ok`. `radio=ABSENT` → SPI0 wiring
   to the CC1101 is wrong, VCC isn't 3.3V, or the module is dead. Check
   GP18/19/16/17 + GDO0 GP20 and the 3V3 rail. This is the most common first
   failure.
2. **Does OOK reach GDO0?** With `radio=ok`, trigger a known OOK remote
   (315/433 MHz, NOT 915) near the antenna. If a hub-fired capture returns
   `NO_RESULT` with a remote pressed, async/raw OOK mode may not be feeding
   GDO0 — suspect `setCCMode(0)`/`setModulation(2)` on your lib version, or the
   frequency.
3. **Local status.** Type `s` → status line (freq, lastproto/bits/value).
4. **Hub link.** Wire SPI1 to the hub's Bus B. From the hub/Medusa:
   `FH_OD_PING_REQ → FH_OD_PING_RESP` (radios_ready bit set); then
   `FH_OD_CC1101_REQ` (freq_khz=433920, dwell_ms=4000) → press a remote →
   `FH_OD_CC1101_RESP` status=OK with decoded {proto,bits,value} in data[0..5].
   A second REQ while one runs → `status=BUSY` (correct, one-at-a-time). Prove
   this blade alone before adding other fired blades.

## Failure decision tree
| Symptom | Likely cause | Action |
|---|---|---|
| radio=ABSENT | SPI0 wiring / 3V3 / dead module | recheck step 1 pins + rail |
| capture NO_RESULT w/ remote pressed | async OOK not on GDO0, or wrong freq | check CCMode/modulation/freq (step 2) |
| hub gets nothing | SPI1 wiring / CS / SPI mode / SPISlave1 | verify SPI1 pins, MODE 1, common ground |
| hub gets byte-0-then-0xff | SPI MODE 0 (should be MODE 1) or SPISlave1 can't stream | set MODE 1; if still failing see FUTURE (pico-sdk port) |
| hub gets garbage frames | CS framing / xfer size mismatch | confirm hub clocks 261 B; common ground |
| compile error SPISlave1 | arduino-pico version differs | confirm onDataRecv/onDataSent on your core |

## Verify-at-bench
- Confirm every GPIO above against YOUR Pico silkscreen before soldering.
- Common ground between the Pico, the hub, and the radio module.
- Prove SPI0 (radio) first, then the SPI1 hub link.
