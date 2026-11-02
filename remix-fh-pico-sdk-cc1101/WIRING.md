# remix-fh-pico-sdk-cc1101 — WIRING

> Pico (RP2040) + CC1101, **native Raspberry Pi Pico C/C++ SDK**. **TWO SPI
> blocks, SPLIT THE OTHER WAY FROM THE ARDUINO BLADE:** hub link on **spi0**
> (slave, copied from the bench-proven GPS blade), radio on **spi1** (master to
> the CC1101). GPn = silkscreen GPIO — VERIFY on YOUR board. CC1101 VCC is
> **3.3V ONLY** (Pico 3V3 pin 36, never 5V/VBUS).
>
> Five-doc set: **WIRING** (this file) · **README** (for people) · **AGENTS**
> (for AI/LLM agents) · **CONTINUITY** (resume-after-a-drop) · **FUTURE**
> (planning). Plus **PICO_LESSONS.md** — the hard-won RP2040 lessons (copied
> verbatim from the GPS master); read it before debugging any link problem.

## SPI-block split — why it differs from the arduino blade
The arduino blade put the radio on SPI0 and the hub link on SPI1. This SDK port
**inverts that**: the hub link stays on **spi0** so it is byte-identical to the
proven GPS pump (same peripheral, same pins, same reset/preload), and the radio
moves to **spi1**. The GPS pump is the one thing we KNOW works on the bench, so
it is not disturbed. (Decision 2026-11-01, see `../remix-PICO-SDK-PORT-PLAN.md`.)

## Hub link — spi0 slave to the P4 Bus B (SAME pins as the GPS blade)
| Pico GPIO | Signal | Dir | Hub side (P4) |
|---|---|---|---|
| GP18 | SCK  (hub drives) | in  | FH_BUSB_SCLK |
| GP16 | MOSI (hub → blade)| in  | FH_BUSB_MOSI (spi0 RX on the slave) |
| GP19 | MISO (blade → hub)| out | FH_BUSB_MISO (spi0 TX on the slave) |
| GP21 | CS   (hub selects)| in  | CC1101 blade = slot 2 = FH_BUSB_CS3 |
| GP6  | IRQ "result ready" (OPTIONAL) | out | a hub Bus-B spare input, or unwired |

- MOSI=GP16 / MISO=GP19 is the **working** GPS convention — do NOT swap them.
- Common **ground** between the Pico and the hub is mandatory.
- **SLOT NOTE:** `FH_BLADE_SLOT_CC1101 = 2` (0-based) = CS3 on the hub.

## Radio bus — CC1101 on spi1 (master)
| CC1101 | Pico GPIO | Notes |
|---|---|---|
| VCC | 3V3 (pin 36) | 3.3V ONLY — 5V destroys it |
| GND | any GND | common ground |
| SCK | GP10 | spi1 SCK |
| MOSI (SI) | GP11 | spi1 TX |
| MISO (SO) | GP12 | spi1 RX |
| CSN | GP13 | chip select |
| GDO0 | GP14 | RX data (capture IRQ) + TX sync |
| GDO2 | GP15 | reserved (unused v0) |

- The two blocks are independent hardware — spi0 (hub) and spi1 (radio) share no
  pin. **Verify GP10–15 don't clash with GP16/18/19/21** and are a valid spi1
  group on your board.
- The vetted driver's `#define SPI_BUS` is set to **spi1** to match (see
  `ELECHOUSE_CC1101_SRC_DRV.cpp`, FH-EDIT).

## Power / RF
- Pico from the hub rail or its own USB. All GPIO 3.3V — no level shifting.
- CC1101: a stable 3.3V; keep the antenna attached before any TX (replay/send).

## SPI modes — NON-NEGOTIABLE, and they are DIFFERENT per block
- **Hub link = spi0 slave MODE 1 (CPOL=0, CPHA=1).** On the RP2040 a MODE-0 SSP
  slave transmits only ONE byte per CS assertion — no pump/preload fix helps.
  The hub drives Bus B in MODE 1 and the proven GPS blade is MODE 1.
- **Radio = spi1 master MODE 0.** The CC1101 is a MODE-0 device; the driver sets
  this internally. This is unrelated to the hub-link mode — do NOT unify them.

## Bench bring-up order
1. **Boot + radio present.** Flash; LED does 1 blink (boot), then 2 (core1
   launched). core1 brings up the CC1101 on spi1 and caches `s_radio_ready`.
   If a later `FH_OD_PING_RESP` has the CC1101 bit clear, spi1 wiring to the
   CC1101 is wrong, VCC isn't 3.3V, or the module is dead — check GP10–13 +
   GDO0 GP14 and the 3V3 rail. Most common first failure.
2. **Hub link alone.** Wire spi0 to the hub Bus B. From the hub/Medusa:
   `FH_OD_PING_REQ → FH_OD_PING_RESP` (CC1101 radios_ready bit set) and
   `FH_TYPE_PING → FH_TYPE_PONG`. This proves the copied GPS pump carries on
   this blade before any radio op. If this fails, the problem is the link, not
   the radio.
3. **Does OOK reach GDO0?** Trigger a known OOK remote (315/433 MHz, NOT 915)
   near the antenna, then fire `FH_OD_CC1101_REQ` (freq_khz=433920,
   dwell_ms=4000). `FH_OD_CC1101_RESP` status=OK with {proto,bits,value} in
   data[0..5] = success. `NO_RESULT` with a remote pressed → async/raw OOK not
   feeding GDO0 (suspect CCMode/modulation on your driver build, or the freq),
   OR the capture IRQ not firing (see PICO_LESSONS: inert capture).
4. **RAW_OOK dump.** Fire action 2 → the blade streams `FH_OD_DUMP_IND` chunks
   (5 B/edge: u32 dur_us LE + u8 level), ONE chunk per hub transaction, then the
   summary `FH_OD_CC1101_RESP` last. Confirm the hub ingests chunks-then-RESP.
5. **REPLAY / ext-TX.** Action 3 re-transmits the last capture; ext-band CC TX
   (0x34) transmits an author-specified OOK triple. Antenna attached first.
   A second REQ while one op runs → `status=BUSY` (correct, one-at-a-time).

## Failure decision tree
| Symptom | Likely cause | Action |
|---|---|---|
| PING_RESP CC1101 bit clear | spi1 wiring / 3V3 / dead module | recheck step 1 pins + rail |
| hub gets nothing on PING | spi0 wiring / CS / SPI mode | verify spi0 pins, MODE 1, common ground (step 2) |
| hub gets byte-0-then-0xff | spi0 set to MODE 0 (must be MODE 1) | set MODE 1 — see PICO_LESSONS |
| capture NO_RESULT w/ remote | async OOK not on GDO0, wrong freq, or IRQ not armed | check driver CCMode/freq; confirm capture_start enables GDO0 IRQ |
| hub never completes a RAW_OOK | chunk pacing / DUMP_IND ingest | confirm one chunk/transaction + RESP last |
| garbage frames | CS framing / xfer size | confirm hub clocks 261 B; common ground |

## Verify-at-bench
- Confirm every GPIO above against YOUR Pico silkscreen before soldering.
- Common ground between the Pico, the hub, and the radio module.
- Prove the hub link (spi0) first, THEN the radio (spi1) — the opposite order
  from debugging the arduino blade, because here the link is the trusted part.
