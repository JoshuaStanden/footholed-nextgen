# remix-FH-P4-hub — CONTINUITY  (future Kiro + Joshua, read this first)

> **Purpose:** if a session is lost, this file lets future-Kiro and Joshua pick
> up the FootHoled-REMIX P4-hub work WITHOUT re-litigating decisions or
> re-deriving the architecture. It is the cold-start spine for this folder.
>
> **Date of this snapshot:** 2026-10-25; bench bring-up added 2026-10-30 (§3c).
> **Working folder:** `~/Desktop/remix footholed/` (ALL remix files live here;
> every NEW file is prefixed `remix-`).

---

## 0. The non-negotiable standing rules (do not violate these)
- **Kiro never compiles / flashes / runs the toolchain. Ever.** Firmware is
  written and reasoned by reading. Joshua builds and flashes (ESP-IDF 6.1 +
  arduino-pico) — since 2026-10-30 he does that on this same PC, with the board
  on COM5. Kiro MAY read his build logs and probe COM5 with small Python scripts
  that only send Medusa's own bridge frames (ATTACH/HB/DETACH) — that is how
  the 2026-10-30 bring-up was verified.
- **Working folder is `~/Desktop/remix footholed/`.** New files get the
  `remix-` prefix. Do not scan broad/system parts of this corporate laptop.
- **Whiteboard by default:** reason/draft, but do not write files until Joshua
  says to. (He lifted it for this firmware build explicitly.)
- **Deep self-review before shipping firmware** — progressively deeper passes
  (The Core tiers: abyss → challenger-deep → nickel tour; and X-Files "trust no
  one"). Be HONEST about defects per pass. Distinguish proven-by-reading from
  proven-on-hardware. Do NOT fake depth with extra passes; name bench facts as
  bench facts.
- **Footguns fixed in code, not left for someone to trip over** — but say
  plainly when the naive fix would make it worse and do the right thing instead.
- **No unrequested changes / renames.** Rename only the `.zip`, not load-bearing
  inner folders, unless Joshua wants inner consistency.

---

## 1. Where we are (the big picture, REMIX = a new generation)

FootHoled is being re-architected. The gen1 "FH-Advanced" build had a Waveshare
**S3** as the hub with 3 UART feeds + a 4-radio Pico. REMIX changes two things
at once:
- **The hub is now the ESP32-P4-WIFI6-DB.** The Waveshare-S3 is demoted from
  hub to **HEAD** (the face: it owns its own LCD + touch and renders what the
  hub feeds it).
- **Inter-board transport moves from UART to SPI.** (Joshua was misled on I²C's
  speed; I²C-as-blade-bus is dead. SPI is committed.)

The hub does exactly THREE jobs: **coordinate** peripherals over two SPI buses,
**write** the unified log to microSD, **emit** display data to the head. Plus
hub-local **BLE sniff** on the P4's onboard **C6** (off-bus). (Earlier notes
said "C5" — WRONG; the chip identifies itself as esp32c6. See §3c.)

### The topology (LOCKED)
```
  Waveshare-S3 HEAD  +  XIAO-S3 (2.4G)  +  XIAO-C5 (5G)   <- 3 peers on BUS A
            \                |                 /              (quad-SPI, IRQ-driven)
                       ESP32-P4  = HUB
                             |                                BUS B (conventional SPI)
                     Pico + radio BLADES  (CC1101/nRF24/PN532/SX1262/GPS)
                     fired one-at-a-time; GPS is POLLED (1/5 Hz)
```
Sensor Stick: UNCHANGED, not part of this re-architecture.

---

## 2. Decisions locked this session (do not re-ask)
1. Hub = ESP32-P4-WIFI6-DB. Head = Waveshare-S3 (face only).
2. Bus A = ONE shared quad-SPI bus, 3 ESP32 peers, per-peer CS + IRQ. IRQ-driven
   drain (peer raises IRQ → hub clocks it in). The head is the 3rd peer.
3. Bus B = ONE shared conventional SPI bus, up to 6 Pico+radio blades, CS each.
   Fired one-at-a-time (frozen BUSY contract).
4. GPS stays a **blade** on Bus B, **polled on a schedule** (cadence from
   `cfg.gps_rate`: 1 Hz default / 5 Hz). It does NOT disturb fired radios
   (<0.1% bus occupancy at 5 Hz). The Pico+GPS blade firmware will be rewritten
   for SPI later (NOT yet done — see §4).
5. BLE sniff = hub-local on the P4 onboard **C6** (ESP-Hosted over SDIO).
   LIVE since 2026-10-30 (new NimBLE observer, see §3c).
6. **Pin budget VERIFIED** against the real board (27 free header GPIO; quad
   over GPIO matrix legal at ~26–40 MHz; ample for distilled-event traffic).
   See `../remix-FH-Gen2-PIN-BUDGET-BASELINE.md`.
7. **Framework-first (pass 1):** prove both buses move `fh_protocol`, log to SD,
   keep Medusa working. NO capture interpretation / blade radio logic yet.
8. Medusa (the laptop tool) must keep working on the P4 hub — the USB bridge is
   carried; command relay now targets the Pico **blade** instead of a UART.

---

## 3. What is DONE (this folder) — the P4 hub firmware, pass 1
`remix-FH-P4-hub-firmware/` — a complete ESP-IDF 6.1 project, written by
reading only:
- `components/fh_protocol/` — frozen wire protocol, **byte-identical** to gen1.
- `main/fh_hub_link.{h,c}` — the NEW SPI link layer (both bus masters, Bus-A
  IRQ drain + one decoder per peer, Bus-B fired xfer + scheduled GPS poll, one
  mutex per bus). This is the heart of the UART→SPI port; `route_frame` is the
  gen1 `on_frame` routing verbatim.
- `main/fh_hub_board.{h,c}` — rewritten for the P4: SDMMC 4-bit + optional
  hub-local I2C; NO panel (head owns it).
- `main/fh_hub_ble.{h,c}` — pass-1 stub.
- `main/fh_hub_ui.c` — headless status model (panel calls removed).
- `main/fh_hub_{logger,time,health,config,bridge}.{h,c}` — carried verbatim
  (bridge's host-command relay repointed to the Pico blade).
- `main/fh_hub_main.c`, pins, CMake ×2, `sdkconfig.defaults`.
- `README.md`, `WIRING.md`, this file.

Self-review done: braces balance, includes resolve, callback contract matches,
one-mutex-per-bus concurrency verified by inspection, no stale UART/agg/LCD code.

### 3a. Multi-check fixes applied to the hub (2026-10-25)
Three real issues found+fixed in the multi-check deep pass:
- **#1 blade PONG/heartbeat src_id**: `blade_link_t` gained a `src_id` field; the
  Pico blade slot is set to `FH_SRC_PICO` so its PONG routes to health node 3
  (was hard-coded 0 → silently dropped). `link_send_ping` now also routes a Pico
  PING to its Bus-B slot (was a no-op).
- **#2**: removed a dead `frame_ctx_t` typedef.
- **#3 two-phase blade xfer**: `fh_link_blade_xfer` now does TWO transactions —
  deliver the request, a ~500µs gap (`esp_rom_delay_us`, needs `esp_rom_sys.h`),
  then clock in the response. Reason: a SPI-slave blade stages its reply for the
  NEXT transaction (it cannot answer within the same clocked frame), so a single
  tx+rx would have read stale bytes. This is the correct request→response
  foundation for the fired blades.

### 3b. Head promoted to a health-tracked node (2026-10-25, Medusa redesign)
While redesigning Medusa's status bar we decided the Display indicator must mean
"hub has an ACTIVELY-CONFIRMED BIDIRECTIONAL link with the head" (green), not a
one-way push. That required the head to become a real liveness node:
- **Frozen protocol:** added `FH_SRC_HEAD = 4` to the `fh_src_id_t` enum in
  `fh_frame.h`, applied IDENTICALLY to all 8 vendored copies (md5 re-verified
  identical). The enum comment was broadened: src_id now identifies capture
  senders AND control-liveness nodes (HEARTBEAT/PING/PONG carry src_id too).
  LATER the same session added `FH_SRC_WS_BLE = 5` (the P4 on-board BLE sniffer —
  an ordinary hub-LOCAL capture source, matches the existing `FH_SRCSTR_WS_BLE`
  "ws-ble" string) — again all 8 copies, md5 re-verified (current hash shared by
  all 8). TODO (hub firmware, parked): enable the BLE observer and stamp its
  captures with src 5 so Medusa renders them as `BT`.
- **Frozen protocol (`fh_ondemand.h`):** named the CC1101_REQ `action` sub-field
  values — `FH_OD_CC1101_ACT_LISTEN=0, RX_ONE=1, RAW_OOK=2, REPLAY=3` (was a bare
  `0/1/2/...` comment). Additive (no struct-size change), all 8 copies, md5
  re-verified identical. Enables Medusa's command-bar verb grammar (cc1101 cap/
  replay). TODO (hub+blade firmware): implement REPLAY = hub reads a saved file
  from SD and streams it to the CC1101 blade to transmit (the SD→blade TX path
  does not exist yet).
- **Frozen protocol (`fh_frame.h`):** added frame `FH_TYPE_SSID = 0x04` +
  struct `fh_ssid_t` (39 B: mac[6] + ssid_len + ssid[32]) + its `_Static_assert`
  — all 8 copies, md5 re-verified identical, parser confirms 39 B / type 0x04.
  A daughterboard (Wi-Fi sniffer) emits it OCCASIONALLY per AP (BSSID->full SSID;
  stable mapping, so CAPTURE stays 34 B); the host joins by MAC for EXACT
  duplicate-SSID / rogue-AP detection. The hub ROUTES it: bridge-mirrored to
  Medusa like every frame AND logged to SD via a `FH_TYPE_SSID` case in
  `route_frame` (BSSID in mac column, `ssid=<name>` / `<hidden>` in extra,
  CSV-safe; source_str from the link src_id, defaulting to the 2.4 label since
  fh_ssid_t carries no band). DONE (2026-10-31, concern #3): BOTH sniffer
  firmwares now emit it — `fh_xs3_main.c` / `fh_c5_main.c` extract the SSID
  tag from beacons (subtype 0x8) and probe responses (0x5), bounds-check every
  tag length against the packet, and send one fh_ssid_t per BSSID (first
  sight + on SSID change; hidden APs send ssid_len 0). Separate 128-entry
  SSID table + mutex in each sniffer; capture path untouched.
- **Health model (`fh_hub_health.c/.h`):** `NODE_COUNT 3 → 4`. Everything else
  was already src_id-generic (`idx_of`, aging task, ping path, `fh_health_node`),
  so the single constant widens the table; node 4 is driven purely by the generic
  HUNG→PING→PONG path (NO head-specific timing added — reuses the same
  thresholds).
- **Link routing (`fh_hub_link.c`):** the HEAD peer was carrying `src_id = 0`, so
  `link_send_ping(4,…)` matched no peer and returned false — the health slot could
  never be probed. Fixed: HEAD peer now carries `FH_SRC_HEAD`, so the monitor's
  PING routes to it over Bus A, and a head PONG routes back to health node 4
  (`route_frame` → `fh_health_on_pong(4,…)` → `idx_of(4)=3`). Verified inbound
  paths key off the PAYLOAD src_id, so no capture/heartbeat misclassification.
- **Pins (`fh_hub_pins.h`):** `FH_PEER_HEAD` comment reconciled (head carries
  FH_SRC_HEAD=4, liveness-only).
DEPENDENCY / honest state: node 4 reads **DEAD until head firmware answers the
PING** — that is correct ("head not responding" → Medusa Display red). It flips
to green with ZERO further hub changes once head firmware (see §4.1) exists to
PONG. All proven-by-reading (brace balance, symbol resolution, path trace); NOT
compiled.

### 3c. FIRST HARDWARE BRING-UP (2026-10-30) — proven on the bench
Joshua built + flashed on this PC (board on **COM5**, a CH343 USB-UART). End
state: clean build, stable boot, BLE sniffing live, **Medusa green with live BT
devices** (maker / service / flags / TX power). Fixes, in the order found — each
one is marked `BENCH FIX (2026-10-30)` in the code:

| # | Symptom | Root cause | Fix (file) |
|---|---|---|---|
| 1 | `-Werror=type-limits` in `fh_frame.c:34` | `uint8_t payload_len > 255` always false | line removed, comment left (`fh_frame.c`; synced to ALL 8 copies, md5-identical again) |
| 2 | Nothing after the ROM banner | console was USB-Serial-JTAG; the USB-C goes to a **CH343 on UART0** | console = UART0 (`sdkconfig.defaults`) |
| 3 | Boot loop `assert ... fh_health_node` | Bus B failed before health started → NULL mutex | health starts first; NULL-mutex guards (`fh_hub_link.c`, `fh_hub_health.c`) |
| 4 | `GPIO 0 is conflict` | `spi_bus_config_t {0}` left `data4..7 = GPIO0` | set to -1 (`fh_hub_link.c`) |
| 5 | SD `slot is not available` | `SDMMC_HOST_DEFAULT()` = slot 1 = ESP-Hosted's | `host.slot = SDMMC_HOST_SLOT_0` + LDO_VO4 (`fh_hub_board.c`) |
| 6 | `Bus B init failed: ESP_ERR_NO_MEM` | P4 **SPI3 has 3 CS, SPI2 has 6**; Bus B needs 6 | Bus A = SPI3, Bus B = SPI2 (`fh_hub_link.c`, WIRING.md) |
| 7 | Crash in `sdmmc_host_get_slot_width` | `host.flags =` dropped `DEINIT_ARG`, so a failed (no-card) mount tore down the whole controller incl. ESP-Hosted's slot | keep `SDMMC_HOST_FLAG_DEINIT_ARG` (`fh_hub_board.c`) |
| 8 | `Identified slave [esp32c6] != Expected [esp32c5]` | onboard co-processor is a **C6** | `CONFIG_SLAVE_IDF_TARGET_ESP32C6` |
| 9 | `Req_FeatureControl` timeout, BLE off | C6 runs **old ESP-Hosted fw (0.0.0)**; doesn't know the BT-controller RPC, but starts BT itself | warn and start NimBLE anyway (`fh_hub_ble.c`) |
| 10 | SD retry flood every 2 s | logger re-mounted on every flush | retry at most every 30 s (`fh_hub_logger.c`) |
| 11 | Medusa got nothing | baud only settable in `ESP_CONSOLE_UART_CUSTOM`; DEFAULT silently pinned 115200 | CUSTOM UART0, TX37/RX38, **921600** (`sdkconfig.defaults`) |
| 12 | Hub stayed in MONITOR | port open can reboot the hub; Medusa's one ATTACH was lost | a heartbeat also attaches (`fh_hub_bridge.c`) |
| 13 | Each device seen once, then nothing | old C6 controller reports each advertiser ~once per scan | scan restarted every 10 s; full table replayed once per attach (`fh_hub_ble.c`) |
| 14 | No maker / caps in Medusa | only 3 of 10 `extra` bytes were used | BLE `extra[]` layout below (`fh_hub_ble.c`) |
| 15 | CSV files 0 bytes on the PC (status said `onSD=3543B`) | `fflush` writes data but FAT only records the file SIZE on `f_sync`/close; files only closed at the 128 MB roll | `fsync(fileno(fp))` after every flush (`fh_hub_logger.c`) — bench-verified: CSV has data on the PC |

**BLE CAPTURE (src 5) `extra[10]` layout** — `extra` is opaque/type-specific in
the frozen protocol, so NO protocol change; LEN = FIXED + 10. Medusa decodes it
in `tui.py` `_ble_meta` — keep the two in step:
`[0]` chan (0) · `[1]` addr type · `[2]` GAP event type · `[3..4]` company ID LE ·
`[5]` AD flags · `[6..7]` first 16-bit service UUID LE · `[8]` TX power (int8) ·
`[9]` presence bits (0x01 company, 0x02 flags, 0x04 svc, 0x08 txp, 0x10 name).

**Feed policy:** SD log = one row per device per boot ("seen is seen"). Medusa =
every controller report (≈ continuous, ~45 frames/s with ~45 devices nearby)
plus a full-table replay on each attach.

**Medusa-side fixes made the same day** — see `remix-FH-Medusa/CONTINUITY.md`.

**C6 co-processor UPGRADED (2026-10-30, later the same night):** 0.0.0 → **ESP-Hosted
2.13.0** (matches the hub's host stack). Done over SDIO with Espressif's
`host_performs_slave_ota` example (Partition OTA), from `~/Desktop/newfirmware/`
(unzipped from the offline component store, because `create-project-from-example`
crashes on this install's `file://` registry mirror; OTA example pinned to
`esp_hosted 2.13.0`). Proven after: `Slave firmware version: 2.13.0`, no version
mismatch, no `Req_FeatureControl` timeout, BLE scan starts at ~3.4 s (was ~8.2 s),
Medusa ~110 BT frames/s, 0 CRC errors. Fixes 9 and 13 are now unnecessary but
harmless (kept). To reflash the C6: rebuild `newfirmware/slave` (target esp32c6,
SDIO), copy `network_adapter.bin` into the OTA example, flash the example, then
reflash the hub.

**Still unproven:** anything on Bus A/B (no peers/blades wired); quad drain speed
on SPI3. (microSD now PROVEN with a 32 GB FAT32 card: mounts, logs write.)

### 3d. BUS A CONVERTED TO QUAD SPI SLAVE HD (2026-11-01)
Joshua committed Bus A to **4-lane (quad) SPI** for throughput headroom (future
full-packet capture, not just distilled events). The ordinary full-duplex
`spi_slave` driver is 1-bit only; quad REQUIRES the **SPI Slave HD** driver. So
the whole bus moved to the HD/ESSL model — a REAL re-architecture, documented in
`../remix-BUS-A-QUAD-HD-DECISION.md`:
- **Hub master (`fh_hub_link.c`):** `busA_init` configures each peer as an HD
  device (command/address/dummy = 8 bits, mode 0, HALFDUPLEX) via
  `spi_bus_add_device`; `busA_drain_peer` pulls a peer's staged frame with
  `essl_spi_rddma` (segment Rd_DMA); `link_send_peer` pushes control with
  `essl_spi_wrdma` (segment Wr_DMA). Added `esp_serial_slave_link`
  (idf-extra-component) to REQUIRES + `main/idf_component.yml`.
  **MULTI-CHECK FIX (2026-11-01):** the first draft used `essl_spi_init_dev` +
  an `essl_handle_t` and called rddma/wrdma with a get_packet-style
  (handle, buf, len, &outlen, flags, timeout) signature — WRONG. The real
  `essl_spi_rddma/wrdma(spi_device_handle_t, buf, len, seg_len, flags)` take the
  raw SPI device handle (not the ESSL handle), a seg_len, and NO timeout/outlen;
  they are the SEGMENT-mode family that pairs with the slaves'
  `spi_slave_hd_queue_trans` (segment mode). The ESSL handle/init_dev is only for
  the Append path (`get_packet`/`send_packet`), which we do NOT use — removed.
  `essl_spi_config_t` also has only `tx_buf_size` (not send/recv_buffer_size).
  Verified against the real idf-extra-components `essl_spi.h`.
- **Peers (`spi_slave_hd`):** XIAO-S3, XIAO-C5, Waveshare head link layers all
  rewritten from 1-bit `spi_slave` to quad `spi_slave_hd` (TX chan = staged
  frame for Rd_DMA, RX chan = control via Wr_DMA). Their public link contract
  (pop/on_control) is unchanged, so the capture/render mains are untouched.
- **FROZEN bus params:** mode 0, command/address/dummy = 8 bits, 261 B segments,
  one CS + one IRQ per peer.
- **#1 BENCH ITEM:** bring up **1-bit HD first** (prove ESSL + DMA), THEN widen
  the master's command to QIO and align dummy cycles — Espressif issue #9823
  (DIO/QIO HD dummy-cycle mismatch). This resolved the old "quad master vs 1-bit
  full-duplex slave" seam, which analysis showed WOULD NOT have communicated.

---

## 4. What is NOT done (the next objects — in rough order)
1. **SPI SLAVE firmware for the peers — WRITTEN (2026-11-01), not bench-proven.**
   All three Bus-A peers now have slave firmware: XIAO-S3 (`../remix-FH-XIAO-S3-
   firmware`), XIAO-C5 (`../remix-FH-XIAO-C5-firmware`), and the Waveshare head
   (`../remix-FH-WS-head-firmware`, NEW this session — LCD/touch + link). ALL
   THREE were converted to **SPI Slave HD (quad)** to match Bus A's committed
   4-lane design (see §3d + `../remix-BUS-A-QUAD-HD-DECISION.md`). The head
   answers PING→PONG, so once the HD link is bench-proven health node 4 lights
   green with no further hub change. REMAINING: bench bring-up (1-bit HD → QIO,
   dummy-cycle #9823) and the per-board HARDWARE-VERIFY on the head's LCD/touch.
2. **Pico + GPS blade rewritten for SPI** (Bus B slave) — answers the scheduled
   poll with a frozen 0x02 fix. Finalize the GPS poll REQ opcode here (the hub
   currently sends a placeholder — link.c R5).
3. **On-demand radio blades** — DONE as SEPARATE single-radio blades (REMIX
   one-radio-per-blade, not the gen1 one-Pico-four-radios): the pico-sdk ports
   remix-fh-pico-sdk-cc1101, -nrf24, -sx1262 are built (the arduino originals
   are archived under ../archive/arduino-blades/). The PN532 NFC blade is
   **PARKED** (2026-11-01 — card-reader radios deferred, dedicated tools exist;
   slot 4 stays reserved, see ../remix-PARKED-CARD-READER-RADIOS.md). The hub now
   addresses each by its OWN Bus-B slot via a per-radio map
   (fh_link_slot_for_radio + FH_BLADE_SLOT_CC1101/NRF24/PN532/SX1262 in
   fh_hub_pins.h). REQUESTER (2026-10-31): the hub's on-demand requester
   EXISTS now (main/fh_hub_req.c) — it fires the REQ, polls the blade slot
   with IDLE frames every 250 ms until the matching-seq RESP lands or the
   deadline (job duration + 2 s margin, capped 45 s) passes, writes the result
   to SD (a clean row in /sdcard/logs/results.csv; a CC1101 job ALSO writes
   its own file /sdcard/ondemand/cc1101_<uptime_s>u_seq<N>.log — the user
   requirement: relative timestamp as the filename, no weird CSV fields) and
   lets route_frame mirror the RESP to Medusa. The bridge submits via
   fh_req_submit and synthesizes a BUSY RESP to the host on a slot collision.
   PN532: PARKED by decision (not pending work) — see
   ../remix-PARKED-CARD-READER-RADIOS.md. NOT bench-proven (write-by-reading
   pass — Joshua builds/flashes next; expect a compile-fix pass).
4. **Pass 2 on the hub:** ~~turn on BLE observer~~ (DONE 2026-10-30, §3c);
   ~~sniffers send SSID frames~~ (DONE 2026-10-31, concern #3 — both XIAO
   firmwares extract + emit FH_TYPE_SSID, see §3); serialize the status model
   to the head as a render feed over Bus A; Bus-A throughput upgrade
   (two-phase / multi-frame) ONLY after the fixed-size base form is
   bench-proven.

---

## 5. Open BENCH facts (cannot be resolved by reading — verify first)
- ~~Quad-SPI master API names on IDF 6.1~~ — RESOLVED 2026-10-30: compiles and
  `spi_bus_initialize` succeeds. STILL OPEN: whether a per-transaction
  `SPI_TRANS_MODE_QIO` is needed for the 4-bit read (needs a real peer). (link.c R2.)
- ~~SDMMC default slot = TF slot~~ — RESOLVED: the default is slot 1 (ESP-Hosted's);
  the TF card is slot 0, now set explicitly. STILL OPEN: a real card mounting
  (LDO_VO4 + GPIO45 SD_VDD_EN). (board.c D1.)
- **5 Hz on Joshua's low-cost GPS** — plausible (NEO-6M tops at 5 Hz) but a
  hardware fact; the hub polls at whatever `gps_rate` says.
- **Quad multidrop with 3 ESP32 slaves** — the DQ tri-state discipline is a
  slave-firmware requirement; prove two peers never both drive DQ.
- **Bus B clock is dialed down to 1 MHz for bring-up — RAISE it to 8 MHz once
  the blade links prove out.** `FH_BUSB_CLK_HZ` in `fh_hub_link.c` was dropped
  from 8 MHz to **1 MHz** (BENCH, 2026-10-30) for jumper-wired bring-up. This is
  NOT a mismatch — the master drives the clock and the RP2040 blades (all
  configured for 8 MHz) follow fine at 1 MHz — but it is a deliberate temporary
  value. Once the four SDK blades (gps/cc1101/nrf24/sx1262) answer reliably on
  the bench, raise `FH_BUSB_CLK_HZ` back to `8 * 1000 * 1000` and confirm the
  blades still stream 261 B clean at 8 MHz (GPS already proved 8 MHz on its own
  blade). Leaving it at 1 MHz just makes Bus B needlessly slow; it will not
  break anything. (Cross-checked 2026-11-01: hub MODE 1 / 261 B / two-phase
  staged-response model all MATCH the blade firmwares — the clock is the only
  intentional delta.)

---

## 5b. The EXTERN verb band (Tier 3) — what is wired vs. left to an external author

> NOTE for any future person/AI doing the work: this is the complete state of
> the "extern" verbs. Do not re-plumb what is listed as wired.

**Build policy (Joshua's, 2026-10-31):** the rig ships capture/recon/detect
only. Transmit-disruption verbs (jam/brute/protokill) exist in the command-line
grammar but their implementations are deliberately NOT authored in the remix
build — an external programmer/LLM/agent (Joshua hires one) replaces the
`FH_OD_EXT_STUB` blocks in the blades' `on_hub_frame()`. TX-class verbs (to the
operator's OWN devices) were implemented in-project and WORK.

- **FROZEN CONTRACT:** `remix-FH-Medusa/EXTERN_CONTRACT.md` — the external
  author builds against exactly this. Read it before touching anything.
- **Wired and done (hub side):** routing 0x30–0x37 → CC1101 slot, 0x38–0x3F →
  nRF24 slot (`fh_hub_link.c fh_link_slot_for_radio`), RESP = REQ+1
  (`fh_hub_req.c fh_req_resp_type_for`), job deadline from p3 bytes 10–13
  (`job_duration_ms`, 0 = floor deadline, clamp 30 s). No hub changes needed
  for any future author — the band routes generically.
- **IMPLEMENTED (works end-to-end, simulator-gated):** `cc1101 tx` (0x34,
  raw OOK via the carried transmitCode() core) and `nrf24 tx` (0x3C, payload to
  the RF24 default address). Wire layouts documented in EXTERN_CONTRACT.md §7
  and in the fh_ondemand.h comment block.
- **STILL STUB (external author's slot):** `cc1101 jam` (0x30), `cc1101 brute`
  (0x32), `nrf24 jam` (0x38), `nrf24 protokill` (0x3A). Both blades'
  `on_hub_frame()` default-cases answer `FH_OD_EXT_STUB` — that exact block is
  what the author replaces. 0x36 and 0x3E stay reserved for future authors.
- **Protocol headers:** the extern band lives in `fh_ondemand.h` (md5 synced
  across ALL copies — `36b38a1d...` as of 2026-10-31). Verify the md5 gate
  before/after any header edit.
- **wifi/bt/sigint disruption verbs** (deauth, bt jam, karma, …) have NO
  protocol contract yet — they are dead-ends in Medusa ("external module") on
  purpose. Authoring them needs a new contract first, not the ext band.

---


## 6. Source material (where things live)
- Pin budget baseline: `~/Desktop/remix footholed/remix-FH-Gen2-PIN-BUDGET-BASELINE.md`
- Gen1 hub (the port source): `~/Desktop/FH-Advanced-Waveshare-hub-firmware.zip`
- Gen1 system map: `~/Desktop/FH-SYSTEM-MAP.md`
- Medusa (laptop tool): `~/Desktop/remix footholed/remix-FH-Medusa/` (gen1 zip:
  `~/Desktop/FH-Advanced-Medusa-EXPERIMENTAL.zip`). Run: `python tui_textual.py COM5`
  from the ESP-IDF venv (that venv has textual + pyserial).
- Bench copies 2026-10-30: Joshua worked from `~/Desktop/remix-FH-P4-hub-firmware`
  and `~/Desktop/remix-FH-Medusa`; both were synced back into `remix footholed/`.
- Hardware refs (repos + Waveshare/Seeed docs): the links Joshua pasted at the
  start of this session (FHAdvanced / FootHoled / Sensor-Stick repos;
  ESP32-P4-WIFI6-DB; ESP32-S3-Touch-LCD-3.5; Wio-SX1262 XIAO-S3; XIAO-C5).

---

## 7. How Joshua works (so future-Kiro matches it)
- Teach hardware/electronics in the background; give real numbers + the WHY.
- He pressure-tests, brings datasheets/photos, catches errors — match that with
  honest depth, not reassurance. He gets anxious about uncompiled firmware and
  asks for repeated review passes; "dad will be checking your homework" (a
  third-party AI may re-check). Be genuinely honest about defects found.
- One object at a time; orient first ("we are looking at X, this is what it's
  for, is that your understanding?") before editing. Hold writes until he says.
