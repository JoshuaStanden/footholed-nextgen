# remix-fh-pico-sdk-nrf24 — WIRING

> Pico (RP2040) + nRF24L01(+), native **Raspberry Pi Pico C/C++ SDK**. **TWO
> SPI blocks:** hub link on **spi0** (slave, MODE 1, copied from the bench-proven
> GPS blade), nRF24 radio on **spi1** (master). GPn = silkscreen GPIO — VERIFY
> on YOUR board. nRF24 VCC is **3.3V ONLY** (Pico 3V3 pin 36; 5V kills it).
>
> Five-doc set: **WIRING** (this) · **README** · **AGENTS** · **CONTINUITY** ·
> **FUTURE**. Plus **PICO_LESSONS.md** (verbatim GPS master).

## SPI split — differs from the arduino nrf24 blade
Arduino blade: radio SPI0, hub SPI1. SDK port **inverts** it — hub on spi0
(byte-identical to the proven GPS pump), radio on spi1. Same as cc1101/sx1262.

## Hub link — spi0 slave to the P4 Bus B (SAME pins as the GPS blade)
| Pico GPIO | Signal | Dir | Hub side (P4) |
|---|---|---|---|
| GP18 | SCK  (hub drives) | in  | FH_BUSB_SCLK |
| GP16 | MOSI (hub → blade)| in  | FH_BUSB_MOSI (spi0 RX on slave) |
| GP19 | MISO (blade → hub)| out | FH_BUSB_MISO (spi0 TX on slave) |
| GP21 | CS   (hub selects)| in  | nRF24 blade slot CS |
| GP2  | IRQ "RX/result ready" (OPTIONAL) | out | a hub Bus-B spare input |

- MOSI=GP16 / MISO=GP19 is the **working** GPS convention — do NOT swap.
- CS uses the standard edge-IRQ realign (no RadioLib here, so none of the
  sx1262 blade's CS-polling workaround is needed).
- Common ground between the Pico and the hub is mandatory.

## Radio — nRF24L01(+) on spi1 (master)
| nRF24 | Pico GPIO | Notes |
|---|---|---|
| VCC | 3V3 (pin 36) | 3.3V ONLY — 5V destroys it; **add a 10µF cap** at the module |
| GND | GND | common ground |
| SCK | GP10 | spi1 SCK |
| MOSI | GP11 | spi1 TX |
| MISO | GP12 | spi1 RX |
| CSN | GP13 | chip-select (software-driven in `nrf_shim.c`, active low) |
| CE | GP7 | chip-enable (RX/TX toggle) |
| IRQ | — | NOT wired (the framework polls STATUS); leave unconnected |

- nRF24 control is **CE + CSN** (not a CC1101-style GDO). The 10µF cap across
  VCC/GND matters especially for +PA+LNA modules (brown-out resets otherwise).
- The two SPI blocks share no pin. Verify GP10–13/7 don't clash with the spi0
  group (GP16/18/19/21).

## SPI modes — different per block, both correct
- **Hub link = spi0 slave MODE 1** (CPOL0/CPHA1). RP2040 one-byte-per-CS rule.
- **Radio = spi1 master MODE 0** (CPOL0/CPHA0) — `nrf_shim.c` sets it.

## Bench bring-up order

> **Before you start:** the hub-link half (spi0) is the bench-proven GPS pump —
> trust it. The radio half (spi1 + `nrf_shim.c`) is hand-written and unproven on
> silicon (**R-DRIVER**), so if anything radio-side misbehaves, suspect the
> driver FIRST, not the wiring. The README's "Known limitations (plain English)"
> section explains every `R-*` tag in human terms — read it if a result is
> confusing.

1. **Boot + radio present.** Flash; LED 1 blink (boot), 2 (core1 launched).
   core1 runs `nrf_init(76)`; a RF_CH write/read round-trip is the presence
   check. PING_RESP later shows the nRF24 radios_ready bit.
2. **Hub link alone.** PING→PONG, PING_REQ→RESP over the copied spi0 pump.
3. **SCAN.** `FH_OD_NRF24_REQ` mode=SCAN (0), chan_lo..chan_hi → `RESP` with
   `active_chan` = first channel whose RPD latched (0xFF = none). **No dBm —
   RPD is a 1-bit carrier flag.** Trigger a known 2.4 GHz transmitter on a
   channel in range to confirm.
4. **SNIFF.** mode=SNIFF (1), chan_lo = THE channel, dwell_ms → the blade
   streams `FH_OD_DUMP_IND` chunks (40-byte records: u32 t_ms + u8 len +
   payload[32] + 3 pad), one per hub transaction, summary RESP last. Needs a
   real transmitter on that channel (see R-PROMISC — captures framed payloads
   on the default address, not all traffic).
5. **ext-band TX (0x3C)** to your own nRF24 receiver. A second REQ mid-op →
   `BUSY` (one-at-a-time).

## Failure decision tree
| Symptom | Likely cause | Action |
|---|---|---|
| PING_RESP nRF24 bit clear | nrf_init round-trip failed | check spi1 wiring, 3V3 + cap, CE/CSN pins (step 1) |
| hub gets nothing on PING | spi0 wiring / CS / MODE | verify spi0 pins, MODE 1, ground (step 2) |
| hub byte-0-then-0xff | spi0 set to MODE 0 | set MODE 1 (PICO_LESSONS) |
| SCAN always NO_RESULT | RPD timing / wrong channel / driver bug | confirm a real carrier present; see R-DRIVER |
| SNIFF captures nothing | promiscuous limits / address mismatch | see R-PROMISC — default-address framed payloads only |
| brown-out resets under load | missing/insufficient VCC cap | add/🔼 the 10µF cap at the module |

## Verify-at-bench
- Confirm every GPIO against YOUR Pico + module silkscreen before soldering.
- Prove the hub link (spi0) first, then the radio (spi1).
