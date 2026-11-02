# remix-PARKED-CARD-READER-RADIOS.md — card-reader radios are PARKED

**Decision (2026-11-01, Joshua):** FootHoled will NOT build card-reader-class
radios for now. That means the **PN532 NFC blade** (13.56 MHz) and any other
"card reader" style radio: 125 kHz LF RFID, HF RFID/NFC read/write/copy/emulate,
badge cloners, and the like.

**Why:** dedicated devices that read / write / copy / emulate cards already exist
on the bench. Rebuilding that capability here duplicates working tools and pulls
effort away from what FootHoled is for (Wi-Fi / BLE / sub-GHz / 2.4 GHz / LoRa
recon). **This is a deferral, not a deletion** — it may come back later.

## Status
- The PN532 blade firmware was **never written**. There is nothing to archive.
- Every NFC hook that already exists is **left in place, reserved**, so resuming
  later is "write the blade", not "re-plumb the system". Nothing below needs to
  change while this is parked.

## Reserved hooks (do NOT delete or reuse)
| Where | What | Why it stays |
|---|---|---|
| `fh_ondemand.h` (FROZEN, byte-identical in all 10 copies) | `FH_OD_NFC_REQ = 0x26`, `FH_OD_NFC_RESP = 0x27`, `fh_od_nfc_req_t` / `fh_od_nfc_resp_t` | Frozen wire contract. Editing one copy breaks byte-identity; reusing 0x26/0x27 for something else would collide if NFC returns. |
| `fh_frame.h` (FROZEN) | `FH_SRCSTR_PICO_NFC "pico-nfc"` | Same — frozen. |
| Hub `main/fh_hub_pins.h` | `FH_BLADE_SLOT_PN532 = 4` → Bus-B CS5 (GPIO50) | Slot stays reserved for NFC. Do not hand slot 4 to a different radio without un-parking this decision first. |
| Hub `main/fh_hub_link.c` | `FH_OD_NFC_REQ → FH_BLADE_SLOT_PN532` routing | Harmless: with no blade on CS5 a REQ simply times out. |
| Hub `main/fh_hub_req.c` | NFC deadline (`timeout_ms`) + result-row formatting | Same. |
| Hub `main/fh_hub_logger.c` | slot 4 → `"pn532"` source name | Same. |
| Medusa `medusa/client.py` | `read_nfc()` | Client API kept; the simulator still answers it (`simdemo.py` demo). |
| Medusa `medusa/simulator.py` | fake NFC_RESP (MIFARE Classic 1K) | Keeps the sim demo + protocol path exercised. |
| Medusa `medusa/intents.py` | NFC branch / `classify_card` | Kept for when NFC returns. |
| Medusa `tui.py` | `nfc` verbs — **`read` set to `reserved`** (was `live`) | Changed so the TUI does not offer a command that can only time out on real hardware (no blade). Flip back to `live` when the blade exists. |

## To resume later
1. Un-park here (note the date + reason) and in the hub `CONTINUITY.md`.
2. Write `remix-fh-pico-sdk-pn532/` by copying the proven GPS blade transport
   (spi0 hub link SCK=18/MOSI=16/MISO=19/CS=21, MODE 1 @ 8 MHz, two-phase staged
   response, 261-byte frames, PICO_NO_PICOTOOL=1 + `uf2conv.py`) and the six
   byte-identical shared files, then add the PN532 driver on a second bus.
   Vet the driver licence first (MIT/permissive only — GPL was rejected for nRF24).
3. Answer `FH_OD_NFC_REQ` with `FH_OD_NFC_RESP` per the frozen structs — the hub
   and Medusa already speak it.
4. Set Medusa `tui.py` `"nfc": {"read": "live", ...}` back to `live`.
5. Multi-check per `remix-MULTI-CHECK.md`, then bench.

### Deferred hardening to apply WHEN NFC returns (found in a 2026-11-01 audit; NOT fixed now because the code is parked)
- `medusa/intents.py classify_card`: also gate the SAK matches on UID length
  (MIFARE Classic 1K/4K → 4-byte UID) so a malformed RESP isn't misclassified.
- `medusa/intents.py badge_identify`: check the NFC_RESP `status` first and
  return "no tag" when `uid_len == 0`, instead of classifying empty bytes as a
  generic ISO14443A tag.
Both are cosmetic on a path that cannot run today (no blade); apply them as part
of the un-park, not before.

## Explicitly out of scope while parked
Write / clone / emulate / brute-force of any card or badge. If NFC returns, the
first pass is READ-ONLY (UID + type), matching the frozen `FH_OD_NFC_RESP`.
