# AGENTS.md — remix-fh-pico-sdk-nrf24 (for AI / LLM agents)

Read this BEFORE editing. Root `../AGENTS.md` governs the whole rig (MODE-1
rule, no-compile-here, flash, committed all-blades-to-pico-sdk policy). This
file is the nrf24-specific part.

> **`PICO_LESSONS.md` is in this folder** — a verbatim copy of the GPS master
> (md5 `a39f5c37212e6e9c584f5e5dcf2e5511`). Edit the master, re-copy, keep
> byte-identical.

## What this blade IS
The pico-sdk port of `../archive/arduino-blades/remix-fh-pico-nrf24/`. Hub transport = the proven GPS
pump (spi0); radio = a **hand-written FH-owned nRF24 driver** (`nrf_shim.c`) on
spi1. The arduino blade stays until this is bench-proven, then retires.

## Non-negotiables
1. **The hub pump is COPIED FROM THE PROVEN GPS BLADE — do not redesign it.**
   Full SPI0 reset + 8-byte preload + paired RX/TX + CS-edge realign ISR. This
   blade uses the standard CS-edge GPIO IRQ (like GPS/cc1101) — there is NO
   RadioLib here, so NONE of the sx1262 blade's CS-polling / bank-IRQ
   workaround applies. Do not import it.
2. **Hub link = spi0 slave MODE 1.** Radio = spi1 master MODE 0 (`nrf_shim`
   sets it). Two blocks, never merged. (Split inverted vs the arduino blade.)
3. **core0 is the SINGLE WRITER of the outbound frame.** Only core0 calls
   `stage_frame`. core1 produces into mailboxes (`s_resultbuf`, the dump buffer
   `s_dump_*`, `s_ext_resultbuf`) and core0 drains + paces them. Do NOT stage
   from core1.
4. **core0 NEVER touches spi1 or the nRF24.** PING_REQ `radios_ready` reads the
   cached `s_radio_ready` that core1 sets after `nrf_init`.
5. **`nrf_shim.c` is FH-OWNED and PERMISSIVE — keep it that way.** It was
   hand-written against the Nordic datasheet precisely because the clean
   pico-sdk driver (`AndyRids/pico-nrf24`) is **GPL-2.0** and would force the
   whole project to GPL. Do NOT vendor that driver or any GPL/LGPL nRF24 code.
   If you extend the driver, write the code (datasheet register facts are fine
   to use; third-party GPL source is not).
6. **No real RSSI (R-RSSI).** The nRF24 only has the 1-bit RPD. SCAN reports
   `active_chan`; `resp.rssi` stays 0. Do not invent a dBm.
7. **Promiscuous sniff is a practical approximation (R-PROMISC).**
   `nrf_rx_mode(true)` turns CRC + auto-ack off and uses a STATIC 32-byte
   payload width (NOT dynamic payloads — DPL requires auto-ack, which defeats
   capture; the 2nd multi-check fixed a DPL+AA-off contradiction here). It
   receives on the default pipe-0 address (0xE7E7E7E7E7) only, and has NO
   per-packet length (every dump record is 32 bytes). Full promiscuity + true
   lengths need the 2-byte-address hack; if you add it, keep it in `nrf_shim`,
   not scattered in main.c.
8. **nRF24 is CE + CSN**, CSN software-driven (not the SPI hardware CS). The IRQ
   pin is not wired — the framework polls STATUS. Keep it that way unless you
   add an ISR deliberately.
9. **Verb logic carried from the arduino framework blade** (SCAN RPD sweep,
   SNIFF capture-to-dump, ext TX). Do not change the wire behavior without a
   bench.

## Known bench risks (see main.c KNOWN RISKS)
- **R-DRIVER:** `nrf_shim.c` is unproven on silicon — the most likely first bug
  is a register/bit mismatch or a CE/standby timing issue. Bench against a known
  nRF24 transmitter first.
- **R-PROMISC:** sniff captures default-address framed payloads, not all traffic.
- **R-RSSI:** channel presence only, no dBm.

## Review discipline
Subject to `../remix-MULTI-CHECK.md`. "Compiles" = compile-correct-by-reading,
residual unknowns named. Because `nrf_shim.c` is from-scratch code, it warrants
a closer register-by-register read than a vendored driver would. Docs travel
with the edit.
