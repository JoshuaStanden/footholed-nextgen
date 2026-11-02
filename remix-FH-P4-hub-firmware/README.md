# remix-FH-P4-hub-firmware — the ESP32-P4 HUB

> **What this is:** the FootHoled-REMIX **hub** firmware for the
> **Waveshare ESP32-P4-WIFI6-DB**. The P4 is the coordinator / bus master. It
> does **three jobs** and nothing else:
>
>   1. **Coordinate** the peripheral devices over two SPI buses.
>   2. **Write** the unified CSV log to the microSD card (single writer).
>   3. **Emit** display data to the Waveshare-S3 **head** (which renders it).
>
> Plus one hub-local extra: **BLE sniffing on the P4's onboard ESP32-C6**
> (off-bus, over SDIO via ESP-Hosted). **Live on hardware since 2026-10-30.**
>
> **Status (2026-10-30 bench):** builds clean on ESP-IDF 6.1 and boots stable
> on the real P4-WIFI6-DB. Both SPI buses come up (no peers/blades wired yet);
> BLE sniffing runs; **Medusa** attaches and shows live `BT` devices with
> manufacturer / service / flags. microSD not yet tested with a card. No Wi-Fi
> capture interpretation or blade radio logic yet — those plug in later. See
> `CONTINUITY.md` §3c for every bench fix and what is still unproven.

---

## The architecture — "the hardware is the framework / the pipeline"

The hub does not know or care what a peer or blade *does*. It knows how to move
`fh_protocol` frames across two SPI buses and route them. Capability is
**modular** — it plugs in by speaking the frozen protocol; the hub does not
change when capability is added.

```
   Waveshare ESP32-S3 Touch-LCD 3.5  = HEAD (face: owns its own LCD + touch)
   XIAO ESP32-S3                     = 2.4 GHz Wi-Fi sniffer peer
   XIAO ESP32-C5                     = 5 GHz Wi-Fi sniffer peer
        \        |        /
         \       |       /     BUS A  (SPI3): ONE shared QUAD-SPI bus, IRQ-driven
          \      |      /      per-peer CS + per-peer IRQ. Time-sliced.
           v     v     v        A peer raises IRQ -> hub clocks it in.
            ESP32-P4  = HUB / bus master (coordinate + log + feed the head)
                 |                         + hub-local BLE sniff (onboard C6)
                 |   BUS B  (SPI2): ONE shared CONVENTIONAL SPI bus, CS per blade
                 v   fired one-at-a-time (BUSY contract); GPS blade is POLLED.
            BLADES — Pico + radio (CC1101 / nRF24 / PN532 / SX1262 / GPS ...)
```

- **Bus A (peers):** quad-SPI, high-bandwidth, **IRQ-driven**. The Wi-Fi
  sniffers push the heaviest (reduced) traffic, so they earn the fat bus. One
  `fh_decoder_t` per peer. The head is a peer too — it *receives* a render feed.
- **Bus B (blades):** conventional SPI, low-rate, **fired one-at-a-time**. A
  radio op is a bounded request/response (not a raw-sample stream). The **GPS
  blade** is the one periodic source — the hub **polls** it on a schedule
  (cadence from `cfg.gps_rate`: 1 Hz / 5 Hz). At 5 Hz that is ~16 B / 200 ms =
  <0.1% bus occupancy, so it never disturbs fired radio ops sharing Bus B.
- The two buses are **separate SPI host controllers** → they run concurrently
  with no lock between them. Within a bus, one dedicated mutex serializes every
  transaction (the carried gen1 concurrency rule: new sink → new mutex).

---

## The frozen protocol (unchanged)

`components/fh_protocol/` is carried **byte-identical** from the gen1 hub
(`fh_frame.h` / `fh_ondemand.h` / `fh_frame.c`). The frame format
(`SYNC A5 5A / TYPE / LEN / PAYLOAD / CRC16-CCITT`) is transport-independent —
the `fh_decoder_t` does not care whether bytes arrived off a UART (gen1) or an
SPI transaction (here). **This is the whole point of the port:** only the byte
*source* changed (UART → SPI), not the protocol, the routing, the logger, the
time base, or the health model.

Host test (already proven by execution in gen1, 20 assertions):
```
cd components/fh_protocol
cc -std=c11 -Wall -Wextra -Iinclude -o /tmp/t test/test_fh_frame.c fh_frame.c && /tmp/t
```

---

## Subsystems (main/)

| File | Role | Ported how |
|---|---|---|
| `fh_hub_main.c` | composition root / boot order | adapted to the SPI link |
| `fh_hub_pins.h` | P4 pin map | from the verified pin budget baseline |
| `fh_hub_link.{h,c}` | **SPI link layer** (both buses, IRQ drain, GPS poll) | **new — replaces the gen1 UART aggregator** |
| `fh_hub_board.{h,c}` | board layer (SDMMC 4-bit + optional I2C) | **rewritten — no panel** (head owns it) |
| `fh_hub_logger.{h,c}` | single SD CSV writer | carried verbatim |
| `fh_hub_time.{h,c}` | RTC + GPS 0x02 consumer, `now_utc` | carried verbatim |
| `fh_hub_health.{h,c}` | heartbeat/PING/PONG liveness | carried verbatim |
| `fh_hub_config.{h,c}` | defaults + NVS overrides | carried verbatim |
| `fh_hub_bridge.{h,c}` | **Medusa** USB bridge (monitor↔interactive) | carried; command relay now targets the Pico **blade** |
| `fh_hub_ble.{h,c}` | hub-local BLE sniff (onboard C6, ESP-Hosted) + Medusa src-5 feed | **live** (NimBLE observer; new code, not the gen1 port) |
| `fh_hub_ui.c` | status model + headless USB status line | headless-pure (panel calls removed) |

---

## Build / flash (on YOUR hardware only)

```
idf.py set-target esp32p4
idf.py build
idf.py -p <port> flash monitor      # over the single USB-C cable
```

- **Console = UART0 at 921600** (TX GPIO37 / RX GPIO38), reached through the
  board's onboard **CH343 USB-UART** on the single USB-C (Windows: "USB-Enhanced-
  SERIAL CH343", e.g. COM5). It is NOT USB-Serial-JTAG — that USB-C is not wired
  to the P4's native USB pins. Baud is only settable in Kconfig's
  `ESP_CONSOLE_UART_CUSTOM` mode, so `sdkconfig.defaults` selects CUSTOM/UART0.
  `idf.py monitor` picks the baud up automatically.
- This console is also the **Medusa** transport (Medusa is fixed at 921600).
  Line endings forced **LF-only** both directions so the binary frame stream is
  byte-transparent (the gen1 bridge invariant).
- Only one program can hold the port: close `idf.py monitor` before Medusa.
  Quit the monitor with ONE Ctrl+] (mashing it wedges the monitor).
- Co-processor = **ESP32-C6** (`CONFIG_SLAVE_IDF_TARGET_ESP32C6`); the
  `espressif/esp_hosted` component (2.13.0) is pulled by `main/idf_component.yml`.
- Toolchain: **ESP-IDF 6.1**. Confirm `idf.py --version`.

### Driving the hub from the laptop — Medusa
The hub is headless, so the operator console is **Medusa**
(`remix-FH-Medusa`; run `python tui_textual.py COM5`). Medusa sends `ATTACH` to
flip the hub from MONITOR (human logs) into INTERACTIVE (binary `fh_protocol`
frames) and streams live captures/GPS/heartbeats. The command relay now routes
host on-demand REQ/PING to the **Pico blade** on Bus B (`host → hub → Pico`),
and the Pico's RESP is mirrored back to the host — the round-trip closes with
no extra code.

---

## Verification status — read this before trusting a pin or a call

**Proven on hardware (2026-10-30 bench, P4-WIFI6-DB + PC on COM5):**
- Clean ESP-IDF 6.1 build; stable boot (no panics) with nothing on either bus.
- Both SPI buses initialize (`SPI link up: Bus A (quad, 3 peers) + Bus B (6 blades)`),
  so the quad-SPI master API names compile and run on IDF 6.1.
- ESP-Hosted SDIO link to the onboard C6 comes up; NimBLE passive scan runs.
- Medusa attaches (ATTACH_ACK), receives `FH_TYPE_CAPTURE` src 5 frames, 0 CRC
  errors; ~40+ devices with live RSSI, maker, service, flags, TX power.

**Proven by reading (confident):**
- Frozen protocol headers byte-identical (md5 match); `route_frame` is the
  gen1 routing verbatim.
- Concurrency: one dedicated mutex per bus, held only across the SPI
  transaction, released before the decoder push; no nested locks.

**Still unproven — verify first:**
1. **No SPI slave partner exists yet.** Until the peers/blades run SPI slaves
   speaking `fh_protocol`, a transaction reads pad bytes and finds no SYNC —
   harmless. Whether SPI3 handles the quad read drain at speed is unproven.

(Resolved 2026-10-30: microSD mounts and logs with a 32 GB FAT32 card; the C6
was upgraded to ESP-Hosted 2.13.0 — see `CONTINUITY.md` §3c.)

**Parked / later passes (not bugs — planned):**
- The SPI **slave** firmware for each peer (XIAO-S3, XIAO-C5, head) and each
  blade (Pico+radio, incl. the SPI GPS blade) — separate objects.
- BLE device names to Medusa (they do not fit the 10-byte `extra`; SD log has them).
- Bus-A throughput upgrade (two-phase length header / multi-frame drain) — only
  once the fixed-size base form is bench-proven. (link.c R4.)
- The head **render feed** over Bus A — the status model exists; the serializer
  to `FH_PEER_HEAD` is a later pass.

---

## Standing rules honored here
- **Prove the base form before adding modules** — pass 1 is framework-only.
- **Kiro never builds or flashes.** Joshua builds/flashes; Kiro reads build
  logs and probes the serial port read-only to verify.
- **The protocol is the product** — frozen, byte-identical, transport-agnostic.
- **Footguns fixed in code, not documented for someone to trip over** — the
  concurrency discipline (one mutex per shared bus) is built in from line one,
  not left for a future race to rediscover.

_Pin budget verified against the real ESP32-P4-WIFI6-DB GPIO table 2026-10-25
(see `../remix-FH-Gen2-PIN-BUDGET-BASELINE.md`). First hardware bring-up
2026-10-30: builds, boots, BLE → Medusa proven._
