"""
medusa.intents — Layer 2 reference: intents that COMPOSE Layer-1 atomics.

PROPOSED / reference. This file is the SHAPE of Layer-2 composition, not a
hardware-ready module. Some intents call Layer-1 verbs that EXIST on
MedusaClient today; the rest call verbs that are NOT BUILT YET and therefore
raise a clear NotImplementedError (naming the missing atomic) rather than a
cryptic AttributeError. Nothing here throws by surprise.

  REAL Layer-1 verbs (work today, drive these intents end to end):
    ping()          -> radio_test's readiness, survey_rf
    gps()           -> gps_where, survey_rf
    sample_cc1101() -> (bounded dwell; NOT the unbounded subghz_capture)
    sample_nrf24()  -> nrf24_scan
    read_nfc()      -> badge_identify, badge_read
    lora_tx()       -> lora_send  (TX is the BUILT half of the LoRa link)

  NOT-BUILT verbs these intents reference (each raises NotImplementedError):
    nfc_write, nfc_dump, fetch_dump, cc1101_replay(dump_id),
    radio_test, tracker_scan, lora_listen
  (SHIPPED 2026-10-31: system_state(), cc1101_replay(freq) [replay-last],
   cc1101_raw() [unbounded OOK capture-to-dump], sniff_nrf24() [payload
   capture-to-dump]; intents subghz_capture() and nrf24_log() now real.)
  When a Layer-1 atomic ships, swap the raise for the real call — the
  composition shape above it already shows exactly how to wire it.

The point this file proves: a Layer-2 verb is just Layer-1 calls chained, with
the output of one piped into the next, and a confirm() gate before any ACTIVE
step. No hardware logic lives here; no people/credentials live in Layer 1.

GOVERNING PRINCIPLE (owner's decision, 2026-10-22):
  Layer 1 is the COMPLETE, NEUTRAL hardware surface — every radio's real
  capability exists, INCLUDING transmit (cc1101_tx, nrf24_tx). Transmit is not a
  "bad" function; it is a capability, good or bad only in a context Layer 2+
  supplies. We cannot and do not limit what legitimate users compose. So: make NO
  "bad" functions — make complete, neutral ones — and the intents WE ship here are
  only legitimate compositions (sense/log, provision-your-own, replay-your-own,
  transmit-a-file-you-own). What others build on the neutral Layer 1 is theirs to
  own. This keeps the library complete without the project shipping offense.
"""
from __future__ import annotations
from dataclasses import dataclass, field


def _default_confirm(prompt: str) -> bool:
    """Active-verb gate. A real client (TUI/GUI) replaces this with a UI prompt;
    default is deny, so nothing active ever fires without an explicit yes."""
    return False


@dataclass
class ProfileStore:
    """Layer-2 data (lives in Medusa, NEVER the brain): person -> credential."""
    people: dict = field(default_factory=dict)      # name -> bytes (badge creds)

    def creds(self, person: str) -> bytes:
        if person not in self.people:
            raise KeyError(f"no profile for '{person}' — add to the profile store")
        return self.people[person]


class Intents:
    """Composed tasks over a MedusaClient (or any object exposing the atomics)."""
    def __init__(self, client, profiles: ProfileStore | None = None, confirm=None):
        self.c = client
        self.profiles = profiles or ProfileStore()
        self.confirm = confirm or _default_confirm

    # ---- badge family -------------------------------------------------------
    def badge_identify(self):
        """Flipper-style IDENTIFY-THEN-BRANCH: read the presented card, classify
        it, and return the card type + ONLY the actions that make sense for it.
        You do not pre-pick a card type — the PN532 tells us what it is.

        The PN532 reports ATQA + SAK + UID on detection; those classify the card:
          SAK 0x08 / 4-byte UID         -> MIFARE Classic 1K
          SAK 0x18                      -> MIFARE Classic 4K
          SAK 0x00 / 7-byte UID         -> MIFARE Ultralight / NTAG
          SAK 0x20 (ISO14443-4)         -> DESFire / smartcard (crypto)
          else                          -> generic ISO14443-A (UID only)
        """
        card = self.c.read_nfc()                      # Layer-1 atomic (passive) — REAL verb
        kind = classify_card(card.get("sak"), card.get("uid", b""))
        return {"uid": card.get("uid"), "type": kind,
                "actions": ACTIONS_FOR.get(kind, ["read_uid"]),
                "note": NOTE_FOR.get(kind, "")}

    def badge_read(self):
        """passive: identify the presented tag."""
        return self.c.read_nfc()

    def badge_write(self, person: str):
        """ACTIVE: write a known person's credential to the presented blank.
        Composition: profile lookup -> confirm -> nfc_write."""
        creds = self.profiles.creds(person)
        if not self.confirm(f"WRITE {person}'s badge credential to the presented tag?"):
            return {"status": "cancelled"}
        raise NotImplementedError("Layer-1 atomic nfc_write() is not built yet — PN532 write path unbuilt. This Layer-2 intent is reference-shape until that atomic ships.")

    def badge_copy(self):
        """ACTIVE: copy the presented tag onto the NEXT presented blank.
        Composition: nfc_dump (read) -> buffer -> confirm -> nfc_write (buffer)."""
        raise NotImplementedError("Layer-1 atomic nfc_dump()/nfc_write() is not built yet — PN532 full-dump + write path unbuilt. This Layer-2 intent is reference-shape until that atomic ships.")

    # ---- subghz family ------------------------------------------------------
    def subghz_capture(self, freq_khz: int, secs: int):
        """passive: listen -> dump to file + a shape view. No confirm (sense).
        Built on the real Layer-1 atomic cc1101_raw() (unbounded OOK listen,
        capture-to-dump; blade clamps at 30 s)."""
        r = self.c.cc1101_raw(freq_khz, dwell_ms=secs * 1000)
        dump = (self.c.dumps() or [None])[0]
        return {"resp": r, "dump": dump,
                "edges": dump["records"] if dump and dump.get("records") else []}

    def subghz_replay(self, dump_id: str):
        """ACTIVE: re-transmit a stored capture (your own signal)."""
        raise NotImplementedError("Layer-1 atomic fetch_dump()/cc1101_replay(dump_id) — the dump STORE is not built yet (replay of the LAST capture is live: cc1101_replay(freq_khz); this intent needs the dump store to replay a named capture).")

    def subghz_replay_last(self, freq_khz: int):
        """ACTIVE: re-transmit the blade's LAST captured OOK signal at freq_khz.
        Built on the now-real Layer-1 atomic cc1101_replay(freq_khz)."""
        if not self.confirm(f"RE-TRANSMIT the last captured signal on CC1101 @ {freq_khz} kHz?"):
            return {"status": "cancelled"}
        return self.c.cc1101_replay(freq_khz)

    def subghz_transmit_file(self, path: str, freq_khz: int):
        """ACTIVE: 'play this file on the CC1101'. The payload is generated/owned
        elsewhere and copied over; we just transmit the bytes. Neutral capability
        — the file's content and the user's authorization are the user's to own."""
        with open(path, "rb") as f:
            payload = f.read()
        if not self.confirm(f"TRANSMIT {len(payload)} bytes on CC1101 @ {freq_khz} kHz from {path}?"):
            return {"status": "cancelled"}
        raise NotImplementedError("Layer-1 atomic cc1101_tx() is not built yet — CC1101 transmit atomic unbuilt. This Layer-2 intent is reference-shape until that atomic ships.")

    # ---- nrf24 family (WE ship sense/log only; the tx ATOMIC exists in Layer 1
    #      for completeness, but we compose no offensive intent over it) --------
    def nrf24_scan(self, lo: int, hi: int, dwell_ms: int = 200):
        """passive: scan a 2.4 GHz channel range for activity."""
        return self.c.sample_nrf24(lo, hi, dwell_ms)

    def nrf24_log(self, chan: int, secs: int):
        """passive: dwell on one channel, capture to a dump + shape view.
        Built on the real Layer-1 atomic sniff_nrf24() (payload capture-to-dump)."""
        r = self.c.sniff_nrf24(chan, dwell_ms=secs * 1000)
        dump = (self.c.dumps() or [None])[0]
        return {"resp": r, "dump": dump,
                "payloads": dump["records"] if dump and dump.get("records") else []}

    # ---- GPS / system (radio-agnostic) --------------------------------------
    def gps_where(self):
        """passive: human-readable current location."""
        return self.c.gps()                   # REAL verb (latest streamed fix)

    def radio_test(self):
        """passive: per-radio init/SPI diagnostics (built on ping readiness)."""
        raise NotImplementedError("Layer-1 atomic radio_test() is not built yet — per-radio SPI diagnostic unbuilt; ping() returns readiness today. This Layer-2 intent is reference-shape until that atomic ships.")

    def system_state(self):
        """passive: SD free/used, uptime, config, capture counts."""
        return self.c.system_state()

    def trackers(self):
        """passive/defensive: 'is anything following me' — scan for AirTag/Tile/
        Chipolo/FindMy beacons near me."""
        raise NotImplementedError("Layer-1 atomic tracker_scan() is not built yet — AirTag/Tile beacon scan unbuilt. This Layer-2 intent is reference-shape until that atomic ships.")

    # ---- LoRa / sensor-stick (COMMS, not capture) --------------------------
    # LoRa today is the hub<->sensor-stick LINK. Transmit is CORE to it — the hub
    # has to be able to announce its presence to the stick, so lora_send below is
    # essential, not optional. What is NOT BUILT YET is LoRa-as-capture (listening
    # to the RF environment with LoRa). That is "not a current feature," explicitly
    # NOT ruled out — if/when LoRa becomes a capture radio, add a sniff intent then.
    def lora_send(self, data: bytes):
        """ACTIVE: transmit a payload from the hub to the sensor stick."""
        if not self.confirm(f"SEND {len(data)} bytes over LoRa to the sensor stick?"):
            return {"status": "cancelled"}
        return self.c.lora_tx(data)                 # Layer-1 ACTIVE atomic (built)

    def lora_listen(self, window_ms: int = 0):
        """passive: open an RX window to hear the stick's replies. window_ms=0
        means continuous until closed. Inbound arrives via on_lora_rx."""
        raise NotImplementedError("Layer-1 atomic lora_listen() is not built yet — LoRa RX-window atomic unbuilt; lora_tx() exists (TX is the built half). This Layer-2 intent is reference-shape until that atomic ships.")

    # ---- survey (sense-only aggregate) -------------------------------------
    def survey_rf(self):
        """passive: fire the sense radios in sequence, aggregate (BUSY-serialized
        by the brain, so these run one after another automatically)."""
        out = {}
        out["ping"] = self.c.ping()
        out["gps"] = self.c.gps()
        # each of these is a Layer-1 passive atomic; add as they become available
        return out


# --------------------------------------------------------------------- NFC branch
# Classify a 13.56 MHz card from the PN532's SAK byte + UID length, then offer
# only the actions that make sense. (125 kHz LF cards need a different radio —
# see ROADMAP "Hardware gaps".)

def classify_card(sak, uid: bytes) -> str:
    n = len(uid) if uid else 0
    if sak == 0x08:
        return "MIFARE_CLASSIC_1K"
    if sak == 0x18:
        return "MIFARE_CLASSIC_4K"
    if sak == 0x00 and n == 7:
        return "NTAG_ULTRALIGHT"
    if sak == 0x20:
        return "DESFIRE"
    return "ISO14443A_GENERIC"


ACTIONS_FOR = {
    "MIFARE_CLASSIC_1K": ["read_uid", "read_sectors(keys)", "write_blank"],
    "MIFARE_CLASSIC_4K": ["read_uid", "read_sectors(keys)", "write_blank"],
    "NTAG_ULTRALIGHT":   ["read_uid", "read_pages", "write_pages"],
    "DESFIRE":           ["read_uid"],                      # crypto: no clone
    "ISO14443A_GENERIC": ["read_uid", "copy_uid(magic_card)"],
}

NOTE_FOR = {
    "MIFARE_CLASSIC_1K": "Sector data needs the sector keys. UID copy needs a magic/UID-changeable card.",
    "MIFARE_CLASSIC_4K": "Sector data needs the sector keys. UID copy needs a magic/UID-changeable card.",
    "NTAG_ULTRALIGHT":   "Pages are writable; good for provisioning your own blanks.",
    "DESFIRE":           "Cryptographic — cannot be cloned. Use the access system's own enrollment.",
    "ISO14443A_GENERIC": "Reader likely checks UID only; copying UID needs a magic card.",
}
