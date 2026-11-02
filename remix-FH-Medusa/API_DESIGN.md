# FH_Medusa — API DESIGN (forward-looking) — two composable layers

> STATUS: PROPOSED / design-ahead. Only 6 Layer-1 verbs are implemented today
> (client.py: ping/gps/sample_cc1101/sample_nrf24/read_nfc/lora_tx). Everything
> else here is a target, not built. Nothing is real unless it's in code + selftest.
>
> SOURCING: derived from THIS device's radios (CC1101 / nRF24 / PN532 / SX1262 /
> NEO-6M) + our frozen fh_ondemand.h. Structural organization (domain-grouped
> menu, per-radio scan+view, Tools/Settings template, tracker-detect, radio-test)
> was INFERRED from a menu tree the owner pasted — NOT from reading any repo.
> Offensive items (jammers, deauth, spoof/pairing-spam, flood, karma, evil-twin,
> credential harvest, PMKID) are DELIBERATELY EXCLUDED per our standing guardrails.

---

## The core principle: Layer 2 COMPOSES Layer 1

- **Layer 1 = atomic do-functions** on the brain, one per hardware action. They
  take parameters, return a result, and are tagged **passive** (sense) or
  **active** (emit/write → confirm first). They know nothing about people or tasks.
- **Layer 2 = intents** in Medusa that CHAIN Layer-1 calls and add data/context.
  `copy_badge()` is literally `read badge` → `write badge` with the read's output
  piped into the write. No new hardware verb — just composition + a confirm gate.

```
  copy_badge()                       [Layer 2 intent, Medusa]
     ├─ nfc_read()        -> creds   [Layer 1 atomic, passive]
     ├─ (confirm: write these creds to the presented blank?)
     └─ nfc_write(creds)             [Layer 1 atomic, ACTIVE]
```

That piping — output of one atomic becomes input of the next — is the pattern for
every Layer 2 verb. Build atomics well and the intents are cheap to assemble.

---

## LAYER 1 — atomic verbs (the brain's do-functions)

### System / meta
| verb | p/a | status |
|------|-----|--------|
| `ping()` -> readiness + radios-ready bitmap | passive | **built** |
| `radio_test()` -> per-radio SPI/init diagnostics (built on ping) | passive | proposed |
| `system_state()` -> SD free/used, uptime, config, capture counts | passive | **built** (2026-10-31: hub-answered 0x2D/0x2E; RESP = 74 B stats+cfg+fix) |
| `gps_now()` -> current fix (lat/lon/sats/fix_qual) | passive | proposed |
| `list_dumps()` / `fetch_dump(id)` -> enumerate / stream stored captures | passive | proposed |
| `set_config(key, val)` | active | proposed |

### WiFi (XIAO-C5 5 GHz, XIAO-S3 2.4 GHz) — passive only, by design
| verb | p/a | status |
|------|-----|--------|
| `wifi_scan(band)` -> AP/station list | passive | (stream exists via captures) |
| `wifi_monitor(band, chan, dwell)` -> frame capture to dump | passive | proposed |
| (event) `on_capture` -> live capture rows | passive | **built** |

### BLE (hub internal radio) — passive only
| verb | p/a | status |
|------|-----|--------|
| `ble_scan(dwell)` -> device list + RSSI | passive | (stream via hub BLE) |
| `ble_sniff(dwell)` -> passive traffic/RSSI to dump | passive | proposed |
| `tracker_scan()` -> detect AirTag/Tile/Chipolo/FindMy beacons near me | passive | proposed (defensive) |

### 2.4 GHz (nRF24)
| verb | p/a | status |
|------|-----|--------|
| `sample_nrf24(lo, hi, dwell)` -> active-channel/RSSI | passive | **built** |
| `nrf24_sniff(chan, dwell)` -> dwell one channel, capture payloads | passive | **built** (2026-10-31: `sniff_nrf24()` — mode=1, capture-to-dump via DUMP_IND chunks) |
| `nrf24_shape(lo, hi)` -> energy-per-channel for the strip view | passive | proposed |
| `nrf24_tx(chan, payload)` | active | **built** (2026-10-31: ext band 0x3C IMPLEMENTED — transmit to your own receiver, default RF24 address; repeats cap 16) |
| `nrf24_jam(chan, ms)` / `nrf24_protokill(...)` | active | **extern band wired** (0x38/0x3A; STUB until the external author's firmware — command-line only) |

### SubGHz (CC1101)
| verb | p/a | status |
|------|-----|--------|
| `sample_cc1101(freq, dwell)` -> bounded-dwell capture + rssi | passive | **built** |
| `cc1101_listen(freq, dwell)` -> unbounded capture-to-dump | passive | **built** (2026-10-31: `cc1101_raw()` — action=2 RAW_OOK, DUMP_IND chunks; blade clamps 30 s) |
| `cc1101_shape(freq, dwell)` -> signal-present-over-time (1-D strip) | passive | proposed |
| `cc1101_tx(freq, payload)` -> transmit a payload (own devices) | active | **built** (2026-10-31: ext band 0x34 IMPLEMENTED — raw OOK via transmitCode(), same core as REPLAY) |
| `cc1101_jam(freq, ms)` / `cc1101_brute(...)` | active | **extern band wired** (0x30/0x32; STUB until the external author's firmware — command-line only) |
| `cc1101_replay(freq_khz)` -> re-transmit the blade's LAST capture (own) | active | **built** (2026-10-31: action=3) |
| `cc1101_replay_from(seq)` -> re-transmit a SPECIFIC capture from Medusa's store (own) | active | **built** (2026-10-31: `cc1101 replay <freq> <seq>` — fires the implemented 0x34 TX verb with the stored triple; the blade need not still hold the capture) |

### NFC (PN532) — the badge radio (read + write)
| verb | p/a | status |
|------|-----|--------|
| `read_nfc(timeout)` -> UID/type | passive | **built** |
| `nfc_dump(timeout)` -> full tag content (all blocks/pages) | passive | proposed |
| `nfc_write(bytes)` -> write to the presented tag | active | proposed |
| `nfc_emulate(profile)` -> present as a tag | active | proposed |

### LoRa (SX1262) — sensor-stick link
| verb | p/a | status |
|------|-----|--------|
| `lora_tx(data)` | active | **built** |
| `lora_listen(window_ms)` | passive | proposed |
| (event) `on_lora_rx` | passive | **built** |

---

## LAYER 2 — composed intents (Medusa chains the atomics)

### Badge / NFC family (the worked composition example)
| intent | composes | confirm? |
|--------|----------|----------|
| `badge.read()` | `nfc_read` → pretty-print credential | no (passive) |
| `badge.write(person)` | profile lookup → `nfc_write(person.creds)` | YES (active) |
| `badge.copy()` | `nfc_dump` → hold in buffer → `nfc_write(buffer)` | YES (write step) |
| `badge.clone(to_blank)` | `nfc_dump` → store profile → `nfc_write` to a blank | YES |
| `badge.identify()` | `nfc_read` → match against known-credential formats | no |

### SubGHz family
| intent | composes | confirm? |
|--------|----------|----------|
| `subghz.capture(freq, secs)` | `cc1101_raw()` (capture-to-dump) + dump view | no |
| `subghz.replay(dump)` | `fetch_dump` → `cc1101_tx` (own signal) | YES |
| `subghz.transmit_file(path, freq)` | load bytes from a file → `cc1101_tx` ("play this file") | YES |

### LoRa family (COMMS to the sensor stick — not capture)
| intent | composes | confirm? |
|--------|----------|----------|
| `lora.send(data)` | `lora_tx` — hub announces/relays to the stick (CORE to the link) | YES |
| `lora.listen(window_ms)` | `lora_listen` — open an RX window for the stick's replies | no |
| (event) `on_lora_rx` | unsolicited inbound from the stick | — |

### Survey / recon (sense-only, aggregate)
| intent | composes | confirm? |
|--------|----------|----------|
| `survey.rf()` | fire all SENSE verbs in sequence, aggregate to one report | no |
| `survey.wardrive()` | continuous `wifi_monitor` + `gps_now` stamp → SD (our core loop) | no |
| `survey.trackers()` | `tracker_scan` → "is anything following me" (defensive) | no |

### Analysis (host-only, no device burden)
| intent | composes | confirm? |
|--------|----------|----------|
| `analyze(dump)` | DSP features + optional interpretation layer | no |
| `gps.where()` | `gps_now` → human-readable location | no |

---

## Menu SPINE (shared tree; TUI + field GUI are two renderers of this)
Domain-grouped, mirroring the hardware. Each radio: Scan → Shape-view → (active
verbs, confirm-gated). Plus Tools + Settings + About as plain housekeeping.
```
Main
├── WiFi      (C5 5 GHz / S3 2.4 GHz)  scan · monitor · (passive only)
├── BLE       scan · sniff · tracker-detect
├── 2.4GHz    (nRF24) scan · shape · sniff · [tx*]
├── SubGHz    (CC1101) listen · shape · [tx* · replay*]
├── NFC       (PN532) read · dump · [write* · emulate*]      ← badge family
├── LoRa      (SX1262) send/tx · listen   (comms link to the sensor stick)
├── GPS       fix state · sats · where
├── Tools     radio-test · system-state · dumps · serial-passthrough · firmware-update
├── Settings  brightness · timeout · rotation · color-mode · device-info
└── About
```
`*` = active/has-an-effect → the renderer confirms before issuing.

## Rules that keep this sane
- Build the ATOMIC (Layer 1) first, then compose the INTENT (Layer 2). Never put
  hardware logic in Layer 2 or people/credentials in Layer 1.
- Extend fh_ondemand.h → then the atomic verb → then the intent. Header first.
- Field handheld exposes a SPARTAN subset of Layer 2; Medusa exposes it all.
  Same contract, unequal clients.
- Excluded by guardrail (not in any layer): jammers, deauth, floods, spoof/
  pairing-spam, karma, evil-twin, captive-portal credential harvest, PMKID.
