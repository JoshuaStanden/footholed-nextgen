# remix-fh-pico-sx1262 — WIRING

> Pico (RP2040) + Seeed **Wio-SX1262**. **TWO SPI blocks:** SPI0 master to the
> LoRa module, SPI1 slave to the P4 hub (Bus B). GPn = silkscreen GPIO — VERIFY
> on YOUR Pico + the Wio-SX1262 header. Wio-SX1262 is **3.3V**.
>
> The five-doc set: **WIRING** (this file) · **README** (people) · **AGENTS**
> (AI/LLM) · **CONTINUITY** (resume) · **FUTURE** (planning, incl. pico-sdk port).

## Radio bus — Wio-SX1262 on SPI0 (master)
| Wio-SX1262 | Pico GPIO | Notes |
|---|---|---|
| 3V3 | 3V3 (pin 36) | 3.3V; bulk cap near module (TX current spike) |
| GND | any GND | common ground |
| SCK | GP18 | SPI0 SCK |
| MOSI | GP19 | SPI0 TX |
| MISO | GP16 | SPI0 RX |
| NSS (CS) | GP17 | SPI chip-select |
| RESET | GP12 | reset |
| BUSY | GP11 | polled during ops |
| DIO1 | GP10 | primary IRQ / event |
| RF_SW1 | GP9 | **host** antenna-switch control (TX/RX path) |
| DIO3 → TCXO | — | INTERNAL to the module (no host pin); begin() sets 1.6 V |
| ANT (IPEX/U.FL) | — | **attach the antenna before any TX** (+22 dBm PA) |

## Hub link — SPI1 slave to the P4 Bus B
| Pico GPIO | Signal | Dir | Hub side (P4) |
|---|---|---|---|
| GP14 | SCK  (hub drives) | in  | FH_BUSB_SCLK = GPIO31 |
| GP24 | MOSI (hub → blade)| in  | FH_BUSB_MOSI = GPIO32 |
| GP15 | MISO (blade → hub)| out | FH_BUSB_MISO = GPIO33 |
| GP25 | CS   (hub selects)| in  | SX1262 blade = slot 1 = FH_BUSB_CS2 = GPIO47 |
| GP2  | IRQ "RX/result ready" (OPTIONAL) | out | a hub Bus-B spare input, or unwired |

- SPI0 (radio) and SPI1 (hub) are independent hardware — no shared pin. The
  radio pins block the LOWER spi1 group (GP8..15), so the hub link uses the FREE
  **upper spi1 group** GP14/24/15/25 (fix 2026-10-31: the old GP6/4/3/5 map was
  an SPI0 group and would never clock). Verify no clash with GP9–19 and rewire
  the hub side to match.
- Common **ground** between the Pico and the hub is mandatory.
- **SLOT NOTE:** `FH_BLADE_SLOT_SX1262 = 1` (0-based) = CS2 = GPIO47 — LoRa is a
  base blade (slot 1), right after GPS (slot 0).

## Power / RF cautions
- **Antenna attached before TX** — TX into an open load can damage the PA.
- Watch the 3V3 rail under TX (+22 dBm); a bulk cap near the module helps.
- RF_SW1 sense (which level = TX) — VERIFY on your module rev (code uses HIGH=TX).

## SPI mode — NON-NEGOTIABLE
Hub-link SPI slave is **MODE 1 (CPOL=0, CPHA=1)**. On the RP2040 a MODE 0 SSP
slave transmits only ONE byte per CS assertion (byte 0, then 0xff underrun) — no
pump/preload fix helps. The hub drives Bus B in MODE 1 and the bench-proven GPS
blade is MODE 1. See `AGENTS.md` / `../AGENTS.md` §1. Do NOT set SPI_MODE0.

## LoRa is COMMS, not capture
LoRa is the link to the sensor stick — TX is CORE, not a capture sensor. Verbs:
`FH_OD_LORA_TX_REQ` (≤200 B to the stick), `FH_OD_LORA_LISTEN_REQ`
(window_ms, 0 = continuous), and unsolicited `FH_OD_LORA_RX_IND` while listening.
Authorized-use-only; TX gated on the hub/head.

## Bench bring-up order
1. **Boot + radio present.** Flash; USB serial @115200. Expect
   `remix-fh-pico-sx1262 v1 (SPI) boot  radio=ok`. `radio=ABSENT`:
   `-707` = TCXO (but begin() sets 1.6 V, so suspect power/seating first);
   `-2` = SPI0 or DIO/BUSY/NSS/RST wiring (check GP18/19/16/17/12/11/10).
2. **Status.** Type `s` → `radio=ok freq=915.000 listening=no`. Set your region
   band in `g_freqMHz` (US 915 / EU 868) before trusting range.
3. **TX** (needs a second LoRa node or an SDR). From the hub/Medusa:
   `FH_OD_LORA_TX_REQ` → `FH_OD_LORA_TX_RESP` status=OK, non-zero `toa_ms`. Watch
   RF_SW1 (GP9) go HIGH during TX. **Antenna MUST be attached.**
4. **Listen window + unsolicited RX.** `FH_OD_LORA_LISTEN_REQ` enable=1,
   window_ms=0 → `LISTEN_RESP` listening=1. Another node transmits (matching
   freq/bw/sf/cr/sync) → unsolicited `FH_OD_LORA_RX_IND` (rssi/snr/data).
   enable=0 → stop.
5. **Hub link.** Wire SPI1 (upper group) to the hub Bus B; `FH_OD_PING_REQ` →
   PING_RESP, then a TX/listen through the hub.

## Failure decision tree
| Symptom | Likely cause | Action |
|---|---|---|
| radio=ABSENT, -2 | SPI0 / DIO / NSS / RST wiring | recheck step 1 pins |
| radio=ABSENT, -707 | TCXO / power / seating | check 3V3 + module seat |
| TX_RESP RADIO_FAIL | antenna open / PA fault / RF_SW1 sense | attach antenna; verify RF_SW1 level=TX |
| never see RX_IND | freq/bw/sf/cr/sync mismatch | match both nodes' LoRa params |
| hub gets nothing | SPI1 wiring / CS / SPI mode / SPISlave1 | verify SPI1 upper-group pins, MODE 1, ground |
| hub gets byte-0-then-0xff | SPI MODE 0 (should be MODE 1) or SPISlave1 can't stream | set MODE 1; if still failing see FUTURE (pico-sdk port) |

## Verify-at-bench
- Confirm every GPIO against YOUR Pico + Wio header before soldering
  (GP16/18/19 is a valid spi0 group; GP14/15/24/25 a valid spi1 group).
- Antenna attached before any TX. Common ground Pico ↔ hub.
