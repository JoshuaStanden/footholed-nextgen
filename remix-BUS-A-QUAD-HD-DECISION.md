# remix-BUS-A-QUAD-HD-DECISION.md — Bus A is 4-lane SPI Slave HD (committed)

> Repo-level architecture decision + build record for Bus A (hub ↔ the three
> ESP32 peers: Waveshare-S3 head, XIAO-S3 2.4 GHz, XIAO-C5 5 GHz). Companion to
> `remix-PICO-SDK-PORT-PLAN.md` (that covers Bus B / the Pico blades).

## DECISION (Joshua, 2026-11-01): Bus A is QUAD (4-lane) SPI, SPI Slave HD mode.

**Why 4 lanes, not 1-bit.** Headroom is a deliberate design choice. Today's Bus-A
traffic is distilled events (BLE/Wi-Fi capture rows, tens of bytes) that 1-bit
would carry easily — but the rig must tolerate a *future* load we can foresee:
e.g. "capture ALL packets between one client and its AP," which is orders of
magnitude more data than distilled events. Building the bus for the traffic we
*might* run (not just today's) is the call. 4-lane ≈ 4× the wire rate and, via
the HD DMA channels, the right primitive for bulk transfer. (Owner's analogy:
a 1200 hp daily driver — you may not need it every day, but you want it there.)

## What "quad" FORCES (the real consequence — not just a wider flag)

The ordinary ESP-IDF full-duplex `spi_slave` driver is **1-bit only**. Quad
(QUADWP/QUADHD, 4-bit QIO/QOUT) exists ONLY in the **SPI Slave HD (half-duplex)**
driver (`spi_slave_hd`), supported on S3 / C5 / C6 / P4 (HD v2). So committing to
4 lanes commits the whole bus to the **SPI Slave HD / ESSL protocol model**:

- **Half-duplex, master-driven phases:** command → (address) → (dummy) → data.
  The master issues HD commands (`Wr_DMA` to push, `Rd_DMA` to pull, register
  read/writes, `CMD8` segment terminator). The slave is REACTIVE: it queues
  TX/RX DMA buffers (`spi_slave_hd_queue_trans`) and the master decides when to
  move them. This REPLACES the old "slave stages a frame + raises IRQ, hub
  clocks a 261 B full-duplex transaction" model.
- **The hub master must speak ESSL** (`essl_spi_*` over the SPI master driver),
  NOT a plain `spi_device_transmit`. Per-device `command_bits`/`address_bits`/
  `dummy_bits` on the slave MUST match what the hub issues.
- **Bus A becomes half-duplex.** Fine for our fixed-261-byte framing (each
  direction is a separate DMA transfer anyway), but the IRQ now means "I have a
  frame staged in my TX DMA buffer — come `Rd_DMA` it," not "come clock me."

## KNOWN BENCH RISK (documented up front, Espressif's own)
**Dummy-cycle mismatch in DIO/QIO HD mode — Espressif issue #9823.** The ESP-IDF
SPI Slave HD dual/quad example has a documented master↔slave dummy-cycle
mismatch; 1-bit HD works, but QIO needs the dummy_bits aligned on both sides.
This is THE #1 Bus-A bench item. Bring up Bus A in **1-bit HD first** (prove the
ESSL command/response + DMA path), THEN switch the data phase to QIO and resolve
the dummy cycles. Both are the same driver; only the command's line-width mask
and dummy_bits change.

## Bus-A HD parameters (FROZEN so hub master and all peers agree)
- `mode = 0` (CPOL0/CPHA0) — unchanged.
- `command_bits = 8`, `address_bits = 8`, `dummy_bits = 8` (HD requires each be
  a multiple of 8, ≥ 8). The hub issues these; every peer slot config matches.
- Transfer unit: `FH_MAX_FRAME` (261 B) per DMA segment, zero-padded; the
  resync-safe `fh_decoder_t` skips pad. (Same framing contract as before.)
- One CS + one IRQ per peer (hub inputs). IRQ HIGH = "TX DMA buffer staged."
- HD commands used (ESSL defaults): `Wr_DMA` (hub→peer control, e.g. PING),
  `Rd_DMA` (hub←peer frames), `CMD8` segment terminate. Register/CMD9/CMDA
  unused for now.

## Build order (this is a 5-surface change; dependency-ordered)
1. **XIAO-S3 + XIAO-C5 slave links** → rewrite `fh_*_link.c` from `spi_slave`
   (1-bit FD) to `spi_slave_hd` (quad HD). The link HEADER contract
   (`pop`/`on_control`/`*_link_start`) is UNCHANGED, so the capture `main.c`
   files are untouched. [THIS PASS]
2. **Hub Bus-A master** → `busA_init` + `busA_drain_peer` + `link_send_peer`
   rewritten to the ESSL HD command model (`essl_spi_*`), matching the frozen
   params above. [THIS PASS]
3. **Waveshare-S3 head firmware** → new `remix-FH-WS-head-firmware/`: the same
   `spi_slave_hd` Bus-A peer link + an LCD/touch render framework that consumes
   the hub's display feed and answers PING (health node 4). [THIS PASS — framework]

## Status
Written by reading only (project rule) — NOT bench-verified. The HD/ESSL model is
the materially harder transport; expect a bench bring-up pass, 1-bit-HD before
QIO (see the dummy-cycle risk). All three peers + the hub share the frozen params
above, so once one peer talks, the others follow.
