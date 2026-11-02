# AGENTS.md — remix-FH-XIAO-C5-firmware (for AI / LLM agents)

Read this BEFORE editing the C5. The repo-root `../AGENTS.md` governs the whole
rig; this file carries the C5-specific lore. The C5 is the **twin of the XIAO-S3**
— identical structure, differing ONLY by band. Five-doc set: WIRING / README /
AGENTS (this) / CONTINUITY / FUTURE.

## Non-negotiables (this peer)
1. **This is an ESP32 Bus-A peer, a quad SPI SLAVE HD at MODE 0** (`spi_slave_hd`,
   `sc.mode = 0`). The RP2040 MODE-1 one-byte-per-CS rule does **NOT** apply — that is a
   Pico-blade concern. Root `../AGENTS.md` §1: Bus A peers (ESP32-family) stay
   MODE 0. Do NOT change to MODE 1 or claim the MODE-1 rule applies here.
2. **5 GHz on NEW SILICON is the #1 bench item.** `esp_wifi_set_band_mode(
   WIFI_BAND_MODE_5G_ONLY)` + 5 GHz promiscuous RX need ESP-IDF 6.1 and are the
   predicted first failure. Start at 20 MHz / `WIFI_SECOND_CHAN_NONE`, verify HE
   (11ax) RX before widening (IDF 6.0 had an HE/secondary-channel quirk; 6.1
   status unconfirmed). Non-DFS channel set `{36,40,44,48,149,153,157,161}`; DFS
   needs radar-detect handling (not done).
3. **The ONE real delta vs the S3** (do not lose): band-mode 5G_ONLY + the 5 GHz
   channel list + band nibble **0x5** + `src_id = FH_SRC_C5_WIFI5 (1)`.
   EVERYTHING else is the S3 firmware verbatim.
4. **Bus A = quad SPI Slave HD, FROZEN** — same contract as the S3
   (`../remix-BUS-A-QUAD-HD-DECISION.md`): `spi_slave_hd` segment mode, mode 0,
   command/address/dummy = 8 bits, 261-byte segments; the hub pulls with
   `essl_spi_rddma` and pushes with `essl_spi_wrdma`. Do NOT revert to the 1-bit
   `spi_slave`. Bring up 1-bit HD first, then QIO (#9823) — `fh_c5_link.c` L2.
5. **CS/IRQ on GPIO12/11 are NOT UART0 console pins** — no boot-glitch concern
   like the S3's GPIO43/44, but still verify free on the board.
6. **Capture callback NEVER blocks** (0-tick lock + 0-tick enqueue); the
   SPI Slave HD link task is the only transport path. Capture path carried verbatim
   from gen1; the risk surface is the transport + the 5 GHz bring-up.

## Known bench risks (cannot be resolved by reading)
- 5 GHz on new silicon (above) — #1, predicted first failure.
- QIO dummy-cycle alignment with the hub (Espressif #9823); 1-bit HD first.
- `spi_slave_hd` on SPI2_HOST (the C5 has SPI1=flash + SPI2 only); RX length
  4-byte aligned (264 B); DQ2/DQ3 = GPIO1/GPIO2 placeholders — confirm free pins
  + IDF 6.1 behavior.
- 5 GHz channel list legality / DFS for your regulatory domain.

## Review discipline
Subject to `../remix-MULTI-CHECK.md`. Kiro never compiles/flashes here. Docs
travel with the edit: a transport change updates this file + `CONTINUITY.md`.
Bring the S3 up first (mature silicon), then this C5 (new-silicon risk).
