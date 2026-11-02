# FUTURE.md — remix-fh-pico-sdk-cc1101 (what's next)

Forward notes for this blade and its role in the committed port programme.
Pair with `CONTINUITY.md` (current state) and `../remix-PICO-SDK-PORT-PLAN.md`.

## This blade's place in the plan
cc1101 is the **pilot** of the committed all-blades-to-pico-sdk port. Order:
**cc1101 (this) → nrf24 → sx1262** (sx1262 is hardest — a RadioLib shim). This
blade is the **template** the other two copy: the verbatim GPS pump on spi0, the
single-writer core0 / radio-core1 split, and the vendored-driver edit contract.

## Bench-gated (must prove on hardware before these close)
1. **First build.** Confirm the mixed C/C++ link resolves (main.c C → the C++
   driver via `cc_shim.cpp` extern-"C"). R-LINK.
2. **Hub link carries.** PING→PONG and PING_REQ→RESP over the copied spi0 pump,
   on this blade, before any radio op. (Expected to pass — it's the GPS pump.)
3. **Radio bring-up.** spi1 to the CC1101; PING_RESP shows the CC1101
   radios_ready bit. Then a real OOK capture against a known 315/433 remote.
4. **RAW_OOK dump stream.** The hub ingests `FH_OD_DUMP_IND` chunks (one per
   transaction) then the summary RESP last, in order.
5. **REPLAY / ext-TX** against your own device, antenna attached.
6. **R-SPI2 under load.** Watch whether the GDO0 edge storm during a capture
   starves the hub pump on core0. If it does, implement the documented escalation
   (`irq_set_exclusive_handler(IO_IRQ_BANK0)` on core1 for GDO0).

## Smaller items / follow-ups
- **RSSI method name.** The RESP carries `cc_rssi()` → driver `getRssi()`;
  confirm the exact method on the pinned driver version. Worst case the field is
  0 (honest), not wrong.
- **Optional debug UART.** `FH_PICO_DBG_TX/RX` (uart0 GP0/GP1) are reserved but
  not wired to a console in `main.c` — add a `-DFH_DEBUG_UART=1` path like the
  GPS blade if bench debugging needs it.
- **GDO2 (GP15)** is reserved/unused in v0.
- **Release artifact.** Once bench-proven, add a `releases/` prebuilt `.uf2` the
  way the GPS blade carries one.

## Retirement of the arduino blade
Per the one-flashable-artifact-per-blade rule and the committed plan, the
arduino blade `../archive/arduino-blades/remix-fh-pico-cc1101/` **stays until this SDK port is
bench-proven**, then retires. Do not delete it before then — it is the fallback
if the SDK port hits an un-foreseen wall on silicon.

## Explicitly rejected (do not resurrect)
- Redesigning the hub pump. It is the proven GPS pump; leave it.
- Staging the outbound frame from core1. core0 is the single writer.
- Calling `cc_present()`/any spi1 op from core0.
- Unifying the two SPI modes (hub MODE 1 / radio MODE 0 are both correct).
