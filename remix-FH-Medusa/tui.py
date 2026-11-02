#!/usr/bin/env python3
"""
tui.py — FH_Medusa bench console (three-stream design; see TUI_DESIGN.md).

    python3 tui.py --sim                 # drive it from the simulator (no hardware)
    python3 tui.py /dev/tty.usbmodemXXXX # drive it from a real hub over serial

Layout (TUI_DESIGN.md v2):
  STATUS BAR (always, expanded)  ·  LIVE STREAM (read-only)  ·
  LEFT interaction column (dedup device list + commanded ★ rows, navigable)  ·
  RIGHT detail (full monty of the left selection)  ·  NOW line (streaming/countdown)
  m = menu overlay  ·  c = command bar   (plain keys — alt combos don't decode
  reliably across terminals, so a single press of m / c is the trigger)

Design split: MedusaModel holds all state + event handling (headless-testable);
MedusaView is the curses renderer. The model never imports curses, so it can be
smoke-tested without a terminal (see the --selftest-model flag).
"""
import sys
import time
import threading
from collections import OrderedDict
from medusa import MedusaClient
from medusa.session import Busy, Timeout


# ============================================================ MODEL (headless)
class StatusCell:
    """One status-bar element: display text + health state.
    state ∈ {"ok","warn","bad","idle"} -> green/amber/red/neutral in the view."""
    __slots__ = ("text", "state")
    def __init__(self, text, state):
        self.text, self.state = text, state


class Row:
    __slots__ = ("key", "kind", "label", "seen", "first", "last", "detail", "star")
    def __init__(self, key, kind, label, detail="", star=False):
        self.key, self.kind, self.label = key, kind, label
        self.seen, self.first, self.last = 1, time.time(), time.time()
        self.detail, self.star = detail, star


class MedusaModel:
    """All TUI state. No curses. Feeds off a MedusaClient's events + commands."""
    def __init__(self, client: MedusaClient):
        self.m = client
        self.stream = []                 # raw live-stream lines (read-only), newest last
        self.rows = OrderedDict()        # key -> Row (dedup device list, first-seen order)
        # Selection is IDENTITY-based, not a raw index: `_sel_key` is the key of
        # the highlighted row, so appends never slide the cursor off your item.
        # `_follow` (default ON) rides the newest arrival; pressing ↑/↓ turns it
        # off (cursor holds still so you can inspect); jump-to-newest re-arms it.
        self._sel_key = None
        self._follow = True
        self.now = "streaming…"          # bottom NOW-line
        self.lock = threading.Lock()
        self._cmd_busy = False           # a command-bar job is running off-thread
        self._cmd_gate = threading.Lock()  # atomic one-at-a-time gate for it
        self.STREAM_MAX = 500
        # ---- status-bar state (REMIX redesign; see TUI_DESIGN.md) ----------
        # Per-src_id last-seen monotonic time, so a node that stops beating goes
        # stale (client.nodes alone never forgets). Keyed by wire src_id.
        self._last_seen: dict[int, float] = {}
        self._last_good_t = 0.0          # monotonic time of the last good frame
        self._last_gps_t = 0.0           # monotonic time of the last GPS_FIX frame
        self._last_gps = None            # last GPS fix dict (fix_qual, sats, lat, lon)
        # crc RATE: sample the cumulative codec counter + diff each refresh.
        self._crc_prev = 0               # last-sampled cumulative crc_errors
        self._crc_prev_t = 0.0           # monotonic time of that sample
        self._crc_rate = 0.0             # crc errors/sec over the last sample gap
        # freshness windows (s). Liveness ~2-3x the ~1.5s heartbeat cadence.
        self.FRESH_S = 4.0               # title/peer "flowing" window
        self.GPS_FRESH_S = 4.0           # GPS poll window (aligns to 1Hz; tune for 5Hz)
        self.CRC_WIN_S = 3.0             # crc "rising" look-back window
        # wire events
        self.m.on_capture(self._on_capture)
        self.m.on_gps(self._on_gps)
        self.m.on_heartbeat(self._on_hb)
        self.m.on_summary(self._on_summary)

    # ---- source naming (single source of truth) ---------------------------
    # Wire src_id -> human name for PASSIVE capture lines + device rows. These
    # are the continuously-sniffing sources: the two Wi-Fi sniffers and the hub's
    # on-board BLE (src 5 = "BT"). NO 'pico' (src 3): the Pico only HEARTBEATS
    # (not streamed) — its radio work is FIRED and renders by RADIO name via
    # fire_result() (CC1101/nRF24/NFC/…), never "pico". The head (4) is a render
    # sink, emits nothing. Fallback `src{N}` for anything unexpected.
    _SRC_NAMES = {1: "x-c5(5)", 2: "x-s3(2.4)", 5: "BT"}

    @classmethod
    def src_name(cls, src_id):
        return cls._SRC_NAMES.get(src_id, f"src{src_id}")

    # Stream line colors (black bg assumed). Category -> color token; the
    # renderers map the token to their own API (curses pair / Rich markup).
    #   BT=blue  x-s3(2.4)=green_light  x-c5(5)=green_dark  GPS=yellow
    #   SUM=white  fired radio ops=orange  (anything else=white)
    _SRC_COLORS = {1: "green_dark", 2: "green_light", 5: "blue"}

    @classmethod
    def src_color(cls, src_id):
        return cls._SRC_COLORS.get(src_id, "white")

    # ---- DETAIL decoders (metadata only — we observe announcements, not data) --
    # Wi-Fi frame_type: 802.11 type (bits 2-3) + subtype (bits 4-7). We only need
    # a human label for the common recon-relevant frames.
    _WIFI_FTYPE = {
        0x40: "probe-req", 0x50: "beacon", 0x80: "probe-resp",
        0x00: "assoc-req", 0x10: "assoc-resp", 0x48: "null-data",
    }

    @classmethod
    def wifi_ftype(cls, ft):
        if ft in cls._WIFI_FTYPE:
            return cls._WIFI_FTYPE[ft]
        typ = (ft >> 2) & 0x3
        return {0: "mgmt", 1: "ctrl", 2: "data"}.get(typ, "?") + f"/0x{ft:02x}"

    @staticmethod
    def band_chan(bc):
        """high nibble = band marker (5=5GHz-ish, 2=2.4), low nibble = channel idx.
        We report it human-ish; exact channel mapping is sniffer-defined."""
        band = "5GHz" if (bc >> 4) >= 5 else "2.4GHz"
        return band, (bc & 0x0F)

    @staticmethod
    def _stamp():
        return time.strftime("%Y-%m-%d %H:%M:%S")

    # ---- stream (top, raw) + rows (left, dedup) ----------------------------
    def _push_stream(self, line, color="white"):
        # Stream lines are (text, color) — color set by SOURCE at creation
        # (single source of truth; renderers just map the token). No per-line
        # timestamp: the stream IS "now"; exact time lives in the SD log.
        with self.lock:
            self.stream.append((line, color))
            if len(self.stream) > self.STREAM_MAX:
                self.stream = self.stream[-self.STREAM_MAX:]

    def _touch_row(self, key, kind, label, detail):
        """Dedup: update an existing row in place, or append a new one (first-seen
        order — never reorder, so the cursor never jumps)."""
        with self.lock:
            r = self.rows.get(key)
            if r is None:
                r = self.rows[key] = Row(key, kind, label, detail)
            else:
                r.seen += 1; r.last = time.time(); r.detail = detail; r.label = label
            # BENCH FIX (2026-10-30): the detail pane printed the frame's own
            # count (always 1), not how often Medusa has seen the device. A
            # "{seen}" placeholder is filled from the row's real counter.
            if "{seen}" in r.detail:
                r.detail = r.detail.replace("{seen}", str(r.seen))

    # ---- BLE advertisement metadata (src 5 extra[], see fh_hub_ble.c) ------
    # Bluetooth SIG company IDs: a short list of the ones that dominate real
    # scans. Unknown IDs print as hex so nothing is hidden.
    _BLE_COMPANY = {
        0x004C: "Apple", 0x0006: "Microsoft", 0x0075: "Samsung", 0x00E0: "Google",
        0x0087: "Garmin", 0x0157: "Huami/Amazfit", 0x038F: "Xiaomi", 0x0059: "Nordic",
        0x000F: "Broadcom", 0x0002: "Intel", 0x001D: "Qualcomm", 0x02E5: "Espressif",
        0x0171: "Amazon", 0x0499: "Ruuvi", 0x01DA: "Logitech",
        0x0131: "Cypress", 0x000D: "TI", 0x0030: "ST Micro", 0x009E: "Bose",
        0x012D: "Sony", 0x00C4: "LG",
    }
    # 16-bit service UUIDs worth naming (SIG-assigned + common member UUIDs).
    _BLE_SVC = {
        0x180F: "Battery", 0x180A: "Device Info", 0x180D: "Heart Rate",
        0x1812: "HID", 0x1803: "Link Loss", 0x1802: "Immediate Alert",
        0x181A: "Env Sensing", 0x1816: "Cycling Speed", 0x1826: "Fitness Machine",
        0x184E: "Audio Stream Ctl", 0x1852: "Broadcast Audio", 0x1854: "Hearing Access",
        0xFD6F: "Exposure Notification", 0xFEAA: "Eddystone", 0xFE2C: "Google Fast Pair",
        0xFE9F: "Google", 0xFEED: "Tile", 0xFD5A: "Samsung", 0xFE07: "Sonos",
        0xFCF1: "Google", 0xFE03: "Amazon", 0xFE8F: "CSR",
    }
    _BLE_EV = {0: "ADV_IND (connectable)", 1: "DIRECT_IND", 2: "SCAN_IND (scannable)",
               3: "NONCONN_IND (beacon)", 4: "SCAN_RSP"}
    _BLE_ADDR = {0: "public", 1: "random", 2: "public-id", 3: "random-id"}

    @classmethod
    def _ble_meta(cls, extra):
        """Decode the hub's BLE extra[] (10 B). Missing fields say 'none'."""
        x = bytes(extra) + bytes(10)
        have = x[9]
        out = {"addr": cls._BLE_ADDR.get(x[1], f"type {x[1]}"),
               "ev": cls._BLE_EV.get(x[2], f"type {x[2]}")}
        if have & 0x01:
            cid = x[3] | (x[4] << 8)
            out["mfr"] = f"{cls._BLE_COMPANY.get(cid, 'unknown')} (0x{cid:04X})"
        else:
            out["mfr"] = "none advertised"
        if have & 0x04:
            u = x[6] | (x[7] << 8)
            out["svc"] = f"{cls._BLE_SVC.get(u, 'unlisted')} (0x{u:04X})"
        else:
            out["svc"] = "none advertised"
        if have & 0x02:
            f = x[5]; caps = []
            if f & 0x01: caps.append("limited-discoverable")
            if f & 0x02: caps.append("discoverable")
            caps.append("BLE only" if f & 0x04 else "BLE + Classic")
            out["caps"] = ", ".join(caps)
        else:
            out["caps"] = "no flags (beacon-style)"
        out["txp"] = (f"{x[8] - 256 if x[8] > 127 else x[8]} dBm" if have & 0x08
                      else "not advertised")
        out["name"] = "advertised (in SD log)" if have & 0x10 else "none"
        return out

    def _on_capture(self, c):
        now = time.monotonic()
        with self.lock:
            self._last_seen[c["src_id"]] = now
            self._last_good_t = now
        mac = c["mac"].hex(":")[:17]
        name = self.src_name(c["src_id"])
        self._push_stream(f"CAP {name} {mac} rssi={c['rssi']}", self.src_color(c["src_id"]))
        band, ch = self.band_chan(c["band_chan"])
        if c["src_id"] == 5:
            # BT (BLE): the hub packs advertisement metadata into extra[]
            # (layout owned by fh_hub_ble.c; decoded by _ble_meta below).
            m = self._ble_meta(c["extra"])
            detail = (f"BT (BLE)\n"
                      f"time   {self._stamp()}\n"
                      f"addr   {mac}  ({m['addr']})\n"
                      f"rssi   {c['rssi']} dBm\n"
                      f"adv    {m['ev']}\n"
                      f"mfr    {m['mfr']}\n"
                      f"svc    {m['svc']}\n"
                      f"caps   {m['caps']}\n"
                      f"txpwr  {m['txp']}\n"
                      f"name   {m['name']}\n"
                      f"seen   x{{seen}}")
        else:
            # Wi-Fi: metadata/announcement recon — NOT payload capture.
            detail = (f"Wi-Fi  {name}\n"
                      f"time   {self._stamp()}\n"
                      f"mac    {mac}\n"
                      f"frame  {self.wifi_ftype(c['frame_type'])}\n"
                      f"band   {band}  ch {ch}\n"
                      f"rssi   {c['rssi']} dBm\n"
                      f"seen   x{{seen}}")
        self._touch_row(mac, "device", f"{mac}  {name}  {c['rssi']}dBm", detail)

    def _on_gps(self, g):
        now = time.monotonic()
        with self.lock:
            self._last_gps = g
            self._last_gps_t = now
            self._last_good_t = now
        self._push_stream(f"GPS sats={g['sats']} {g['lat']/1e7:.5f},{g['lon']/1e7:.5f}", "yellow")
        fixq = g.get("fix_qual", 0)
        detail = (f"GPS\n"
                  f"time   {self._stamp()}\n"
                  f"lat    {g['lat']/1e7:.6f}\n"
                  f"lon    {g['lon']/1e7:.6f}\n"
                  f"sats   {g['sats']}\n"
                  f"fix    {'valid' if fixq > 0 else 'acquiring (no fix)'}")
        self._touch_row("gps", "gps", f"GPS  {g['sats']} sats  "
                        f"{'fix' if fixq > 0 else 'acquiring'}", detail)

    def _on_hb(self, h):
        now = time.monotonic()
        with self.lock:
            self._last_seen[h["src_id"]] = now
            self._last_good_t = now
        name = self.src_name(h["src_id"])
        self._touch_row(f"node{h['src_id']}", "node",
                        f"{name}  up{h['uptime_s']}s  heap{h['free_heap_kb']}k",
                        f"node {name} (src_id {h['src_id']})\nuptime {h['uptime_s']}s\n"
                        f"captures {h['capture_count']}  heap {h['free_heap_kb']} KB")

    def _on_summary(self, s):
        with self.lock:
            self._last_good_t = time.monotonic()
        self._push_stream(f"SUM {self.src_name(s['src_id'])} count={s['count']}")

    # ---- commanded ★ rows (focus follows intent) ---------------------------
    def add_command_result(self, label, detail):
        with self.lock:
            key = f"cmd:{time.time()}"
            r = Row(key, "command", label, detail, star=True)
            self.rows[key] = r
            # focus follows intent: pin the highlight to the fired result (by
            # identity) and STOP following newest, so the next stream event
            # doesn't yank the cursor off the thing you just ran.
            self._sel_key = key
            self._follow = False

    def fire_result(self, radio, summary, detail):
        """A FIRED blade op completed. Blade events are commanded actions, not a
        passive feed — so they render by RADIO name (CC1101/nRF24/NFC/…), never
        'pico', and carry a per-ACTION summary: a duration for a timed capture
        (`15 SEC`), or an outcome for a one-shot function (`REPLAYED`, `MIFARE
        1K`). Pushes ONE stream line `CAP {radio} — {summary}` AND the ★ detail
        row. Single source of truth — both TUIs call this, so the two stream
        lines cannot diverge."""
        self._push_stream(f"CAP {radio} — {summary}", "orange")
        self.add_command_result(f"{radio} — {summary}", detail)

    # ---- command bar grammar: <radio> <verb> <args> ------------------------
    # One parser, both TUIs. Maps human verbs onto the EXISTING library calls;
    # firmware-dependent verbs (replay/raw/sniff/…) are RECOGNIZED but return a
    # clear "not implemented yet" instead of a cryptic error or a faked result.
    # Human units accepted: `11sec`, `2s`, `500ms`, bare int = ms.
    # Human-readable one-liner from a RESP dict (NOT a struct dump).
    def _od_status(self, r):
        if not isinstance(r, dict):
            return str(r)
        bits = []
        if "status" in r:
            # Map the fh_od_status_t enum to its human name (0 -> OK, 0x06 ->
            # EXT_STUB, ...) so the detail reads "OK" not a bare integer.
            s = r["status"]
            names = {v: k for k, v in self.m.p.enums["fh_od_status_t"].items()} \
                    if hasattr(self.m, "p") and "fh_od_status_t" in self.m.p.enums else {}
            bits.append(names.get(s, str(s)))
        if r.get("rssi") is not None and r.get("rssi") != 0:
            bits.append(f"rssi {r['rssi']}dBm")
        if r.get("active_chan") not in (None, 0xFF):
            bits.append(f"active ch {r['active_chan']}")
        if "toa_ms" in r:      bits.append(f"toa {r['toa_ms']}ms")
        return "  ".join(bits) if bits else "ok"

    @staticmethod
    def _dur_ms(tok, default_ms):
        if tok is None:
            return default_ms
        t = tok.lower().strip()
        try:
            if t.endswith("ms"):   return int(t[:-2])
            if t.endswith("sec"):  return int(t[:-3]) * 1000
            if t.endswith("s"):    return int(t[:-1]) * 1000
            return int(t)                       # bare number = ms
        except ValueError:
            raise ValueError(f"bad duration '{tok}' (try 11sec, 500ms, 2s)")

    # verb tables: which verbs each radio exposes, and whether live or reserved.
    # Verb states:
    #   "live"     — wired to a real library call, works now
    #   "reserved" — OBSERVATIONAL/capture, honest placeholder until firmware wires it
    #   "extern"   — verb EXISTS in the grammar but its implementation is left to
    #                an external builder (transmit-disruption primitives this tool
    #                does not author). Recognized, not a crash, clear message.
    # All HALEHOUND verbs are PRESENT (grammar is complete); only the primitive
    # authoring differs. Capture/detect/scan/sniff are the device's purpose.
    _VERBS = {
        # radios (direct)
        "cc1101": {"cap": "live", "replay": "live", "raw": "live", "tx": "live",
                   "captures": "live", "spectrum": "reserved",
                   "brute": "extern", "jam": "extern"},
        "nrf24":  {"scan": "live", "sniff": "live", "tx": "live",
                   "spectrum": "reserved", "jam": "extern", "protokill": "extern"},
        # nfc PARKED 2026-11-01 (../remix-PARKED-CARD-READER-RADIOS.md): no blade
        # exists, so "read" is reserved — flip back to "live" when it does.
        "nfc":    {"read": "reserved", "emulate": "reserved", "write": "reserved"},
        "lora":   {"tx": "live", "listen": "reserved"},
        "ping":   {"": "live"},
        "results": {"": "live"},   # job-results log (the hub's on-demand outcomes)
        # WiFi suite
        "wifi":   {"scan": "live", "station-scan": "live", "dupe-ssid": "live",
                   "monitor": "reserved", "probe-sniff": "reserved",
                   "deauth-watch": "reserved", "reconnect-test": "reserved",
                   "deauth": "extern", "beacon-spam": "extern", "auth-flood": "extern",
                   "evil-twin": "extern", "captive-portal": "extern"},
        # Bluetooth suite — the hub's onboard BLE sniffer (src 5) streams the
        # same CAPTURE store as Wi-Fi, so scan is LIVE client-side wiring; the
        # specialised detectors still need BLE fields they don't have yet.
        "bt":     {"scan": "live", "sniff": "reserved", "airtag-detect": "reserved",
                   "stalkerware-detect": "reserved", "whisperpair-scan": "reserved",
                   "beacon": "extern", "jam": "extern", "spoof": "extern",
                   "pair-spam": "extern"},
        # SIGINT suite (capture/recon)
        "sigint": {"wardrive": "live",
                   "eapol": "reserved", "pmkid": "reserved", "saved": "reserved",
                   "karma": "extern"},
        # Tools
        "tools":  {"gps": "live", "sysstate": "live",
                   "serial": "reserved", "fwupdate": "reserved",
                   "touch-cal": "reserved", "radiotest": "reserved"},
    }

    def parse_command(self, line):
        """Parse + run a command-bar line. Returns one of:
          ("fire", radio, summary, detail)  — a fired blade op (→ fire_result)
          ("info", label, detail)           — a non-fire result (ping) (→ add_command_result)
          ("now", message)                  — just set the NOW line (errors / reserved)
        Never raises to the caller — bad input comes back as ("now", msg)."""
        parts = line.split()
        if not parts:
            return ("now", "")
        radio = parts[0].lower()
        verb = parts[1].lower() if len(parts) > 1 else ""
        args = parts[2:]

        if radio == "ping":
            r = self.m.ping()
            return ("info", "ping", f"PING\nstatus {r.status}\nradios {r.radios_ready}")

        if radio == "results":
            rows = self.m.results()
            if not rows:
                return ("now", "results: no on-demand jobs fired yet this session")
            import time as _t
            body = []
            for r in rows[-20:]:
                hhmm = _t.strftime("%H:%M:%S", _t.localtime(r["t"]))
                bits = [f"{hhmm}  {r['req']:<8} {r['status']:<8} {r['elapsed']*1000:6.0f}ms"]
                if "rssi" in r:     bits.append(f"rssi={r['rssi']}")
                if "chan" in r:     bits.append(f"chan={r['chan']}")
                if "toa_ms" in r:   bits.append(f"toa={r['toa_ms']}ms")
                if "cap_len" in r:  bits.append(f"{r['cap_len']}B")
                if "uid" in r:      bits.append(f"uid={r['uid']}")
                body.append("  " + " ".join(bits))
            label = f"results ({len(rows)} jobs)"
            return ("info", label, "On-demand job results (hub requester view)\n" + "\n".join(body))

        table = self._VERBS.get(radio)
        if table is None:
            return ("now", f"unknown module '{radio}' "
                           f"(cc1101/nrf24/nfc/lora/wifi/bt/sigint/tools/ping/results)")
        if verb not in table:
            return ("now", f"{radio}: unknown verb '{verb}' — try {'/'.join(k for k in table if k)}")
        state = table[verb]
        if state == "reserved":
            if radio == "lora" and verb == "listen":
                return ("now", "lora listen: the hub requester routes LORA_LISTEN_REQ (0x2B) — "
                        "but the SX1262 blade firmware for the RX window is not built yet")
            return ("now", f"{radio} {verb}: recognized (capture/recon verb) — "
                           f"not wired yet, pending firmware")
        if state == "extern":
            # Tier 3: extern-band verbs ARE wired (no menu, command-line only).
            # If the radio+verb has a protocol id in the client's ext table, the
            # REQ fires now — the blade answers EXT_STUB (plumbing works, blade
            # implementation not authored) or a real status once authored.
            # Externs with no protocol contract yet (wifi/bt disruption) stay
            # dead-ends here.
            if (radio, verb) in self.m._EXT_VERBS:
                try:
                    p1 = int(args[0]) if args else 0
                    p2 = int(args[1]) if len(args) > 1 else 0
                    p3 = self._dur_ms(args[2] if len(args) > 2 else None, 0)
                    r = self.m.ext_verb(radio, verb, p1=p1, p2=p2, p3=p3)
                    if r["status"] == 0x06:      # FH_OD_EXT_STUB
                        detail = (f"{r['label']} (extern)\n"
                                  f"command  {line.strip()}\n"
                                  f"result   STUB — routed to the blade, but the verb's "
                                  f"implementation is not authored yet "
                                  f"(external module)\n"
                                  f"params   p1={p1} p2={p2} p3={p3}ms")
                        return ("fire", radio.upper(), "STUB", detail)
                    detail = (f"{r['label']} (extern)\n"
                              f"command  {line.strip()}\n"
                              f"result   {self._od_status(r)}\n"
                              f"params   p1={p1} p2={p2} p3={p3}ms")
                    return ("fire", radio.upper(), r["label"], detail)
                except Exception as e:
                    return ("now", f"{radio} {verb}: {e}")
            return ("now", f"{radio} {verb}: recognized — implementation not in "
                           f"this build (external module)")

        try:
            if radio == "cc1101" and verb == "cap":
                freq = int(args[0]) if args else 433920
                dwell = self._dur_ms(args[1] if len(args) > 1 else None, 2000)
                r = self.m.sample_cc1101(freq, dwell)
                secs = dwell // 1000
                detail = (f"CC1101 cap\n"
                          f"command  {line.strip()}\n"
                          f"freq     {freq} kHz\n"
                          f"recorded {secs}s (full {dwell}ms window)\n"
                          f"result   {self._od_status(r)}")
                return ("fire", "CC1101", f"{secs} SEC", detail)
            if radio == "cc1101" and verb == "raw":
                freq = int(args[0]) if args else 433920
                dwell = self._dur_ms(args[1] if len(args) > 1 else None, 5000)
                r = self.m.cc1101_raw(freq, dwell)
                secs = dwell // 1000
                dumps = self.m.dumps()
                latest = dumps[0] if dumps else None
                recs = (latest["records"] if latest and latest.get("records") else [])
                body = "\n".join("  " + x for x in self.m._dump_records_hex(latest["seq"], 8)) if latest else ""
                detail = (f"CC1101 raw capture-to-dump\n"
                          f"command  {line.strip()}\n"
                          f"freq     {freq} kHz\n"
                          f"recorded {secs}s (full {dwell}ms window)\n"
                          f"result   {self._od_status(r)}\n"
                          f"dump     seq {latest['seq']} · {latest['chunks']} chunks · "
                          f"{len(recs)} edges · {latest['bytes']}B"
                          + (f"  ⚠ gap {latest['gap']}" if latest.get("gap") else "")
                          + ("\n" + body if body else "\n  (no edges captured)"))
                return ("fire", "CC1101", "RAW", detail)
            if radio == "cc1101" and verb == "captures":
                # The decode-triple store: every successful capture this
                # session, newest-first — seq, freq, triple, time.
                caps = self.m.cc1101_captures()
                if not caps:
                    return ("now", "cc1101 captures: no captures this session "
                                   "— run `cc1101 cap <freq>` first")
                import time as _t
                body = []
                for c in caps:
                    hhmm = _t.strftime("%H:%M:%S", _t.localtime(c["t"]))
                    body.append(f"  {hhmm}  seq={c.get('seq', '?'):>4}  "
                                f"freq {c['freq_khz']} kHz  proto={c['proto']} "
                                f"bits={c['bits']} value={c['value']}")
                return ("info", f"cc1101 captures ({len(caps)})",
                        "Captured decode triples this session (replayable with "
                        "`cc1101 replay <freq> <seq>`):\n" + "\n".join(body))
            if radio == "cc1101" and verb == "replay":
                freq = int(args[0]) if args else 433920
                if len(args) > 1:
                    # REPLAY-FROM: re-transmit a SPECIFIC captured signal from
                    # Medusa's own decode-triple store — the blade does NOT
                    # need to still hold it (it may have re-captured or
                    # power-cycled since). Fires the 0x34 ext-band TX verb.
                    r = self.m.cc1101_replay_from(int(args[1]))
                    d = bytes(r.get("data", b""))[: max(r.get("len", 0), 0)]
                    proto = d[0] if len(d) > 0 else "?"
                    bits = d[1] if len(d) > 1 else "?"
                    val = int.from_bytes(d[2:6], "little") if len(d) >= 6 else "?"
                    detail = (f"CC1101 replay-from\n"
                              f"command  {line.strip()}\n"
                              f"freq     {freq} kHz · from capture seq {args[1]}\n"
                              f"replayed proto={proto} bits={bits} value={val}\n"
                              f"result   {self._od_status(r)}")
                    return ("fire", "CC1101", "REPLAY", detail)
                r = self.m.cc1101_replay(freq)
                d = bytes(r.get("data", b""))[: max(r.get("len", 0), 0)]
                proto = d[0] if len(d) > 0 else "?"
                bits = d[1] if len(d) > 1 else "?"
                val = int.from_bytes(d[2:6], "little") if len(d) >= 6 else "?"
                detail = (f"CC1101 replay\n"
                          f"command  {line.strip()}\n"
                          f"freq     {freq} kHz\n"
                          f"replayed proto={proto} bits={bits} value={val}\n"
                          f"result   {self._od_status(r)}")
                return ("fire", "CC1101", "REPLAY", detail)
            if radio == "cc1101" and verb == "tx":
                # CC1101 TX (IMPLEMENTED, Tier 3 ext band 0x34): raw OOK transmit
                # for driving your own devices — same transmitCode() core as
                # replay. Syntax: cc1101 tx <freq_khz> <proto> <bits> <value>
                # [repeats]. Value accepts 0x-hex.
                if len(args) < 4:
                    return ("now", "cc1101 tx: need <freq_khz> <proto> <bits> "
                                   "<value> [repeats] — e.g. cc1101 tx 433920 1 24 0x2A2B3C")
                freq = int(args[0])
                proto = int(args[1])
                bits = int(args[2])
                value = int(args[3], 0)
                repeats = int(args[4]) if len(args) > 4 else 3
                r = self.m.cc1101_tx(freq, proto, bits, value, repeats)
                d = bytes(r.get("data", b""))[: max(r.get("len", 0), 0)]
                echo = (f"proto={d[0]} bits={d[1]} "
                        f"value={int.from_bytes(d[2:6], 'little') if len(d) >= 6 else '?'}")
                detail = (f"CC1101 tx\n"
                          f"command  {line.strip()}\n"
                          f"freq     {freq} kHz · proto={proto} bits={bits} "
                          f"value={value} repeats={repeats}\n"
                          f"result   {self._od_status(r)}\n"
                          f"echo     {echo}")
                return ("fire", "CC1101", "TX", detail)
            if radio == "nrf24" and verb == "tx":
                # nRF24 TX (IMPLEMENTED, Tier 3 ext band 0x3C): transmit a
                # 1..32-byte payload to your own receiver. Syntax:
                # nrf24 tx <chan> <payload> [repeats]. Payload accepts 0x-hex
                # (e.g. 0x1A2B3C) or raw text. LoRa's own tx verb is separate.
                if len(args) < 2:
                    return ("now", "nrf24 tx: need <chan> <payload> [repeats] — "
                                   "e.g. nrf24 tx 76 0x1A2B3C")
                chan = int(args[0])
                prow = args[1]
                if prow.startswith("0x"):
                    payload = int(prow, 0).to_bytes((len(prow) - 2 + 1) // 2, "big")
                elif prow.startswith("0b"):
                    payload = int(prow, 0).to_bytes((len(prow) - 2 + 7) // 8, "big")
                else:
                    payload = prow.encode("utf-8")
                if not (1 <= len(payload) <= 32):
                    return ("now", "nrf24 tx: payload must be 1..32 bytes")
                repeats = int(args[2]) if len(args) > 2 else 1
                r = self.m.nrf24_tx(chan, payload, repeats)
                d = bytes(r.get("data", b""))[: max(r.get("len", 0), 0)]
                okN = d[1 + len(payload)] if len(d) > 1 + len(payload) else "?"
                detail = (f"nRF24 tx\n"
                          f"command  {line.strip()}\n"
                          f"channel  {chan} · payload {payload.hex().upper()} "
                          f"({len(payload)} B) · repeats={repeats}\n"
                          f"result   {self._od_status(r)}\n"
                          f"echo     writes acknowledged: {okN}/{repeats}")
                return ("fire", "nRF24", "TX", detail)
            if radio == "nrf24" and verb == "scan":
                lo, hi = int(args[0]), int(args[1])
                dwell = self._dur_ms(args[2] if len(args) > 2 else None, 50)
                r = self.m.sample_nrf24(lo, hi, dwell)
                detail = (f"nRF24 scan\n"
                          f"command  {line.strip()}\n"
                          f"channels {lo}-{hi}  ({dwell}ms/ch)\n"
                          f"result   {self._od_status(r)}")
                return ("fire", "nRF24", f"ch {lo}-{hi}", detail)
            if radio == "nrf24" and verb == "sniff":
                chan = int(args[0]) if args else 76
                dwell = self._dur_ms(args[1] if len(args) > 1 else None, 5000)
                r = self.m.sniff_nrf24(chan, dwell)
                dumps = self.m.dumps()
                latest = dumps[0] if dumps else None
                recs = (latest["records"] if latest and latest.get("records") else [])
                body = "\n".join("  " + x for x in self.m._dump_records_hex(latest["seq"], 8)) if latest else ""
                detail = (f"nRF24 sniff capture-to-dump\n"
                          f"command  {line.strip()}\n"
                          f"channel  {chan} · recorded {dwell}ms\n"
                          f"result   {self._od_status(r)}\n"
                          f"dump     seq {latest['seq']} · {latest['chunks']} chunks · "
                          f"{len(recs)} payloads · {latest['bytes']}B"
                          + (f"  ⚠ gap {latest['gap']}" if latest.get("gap") else "")
                          + ("\n" + body if body else "\n  (no payloads captured)"))
                return ("fire", "nRF24", "SNIFF", detail)
            if radio == "nfc" and verb == "read":
                timeout = self._dur_ms(args[0] if args else None, 2000)
                card = self.m.read_nfc(timeout)
                from medusa.intents import classify_card
                uid = bytes(card.get("uid", b""))[:card.get("uid_len", 0)]
                kind = classify_card(card.get("tag_type"), uid)
                detail = (f"NFC read\n"
                          f"command  {line.strip()}\n"
                          f"tag      {kind}\n"
                          f"uid      {uid.hex(':') if uid else '(none)'}")
                return ("fire", "NFC", kind, detail)
            if radio == "lora" and verb == "tx":
                payload = (" ".join(args)).encode()
                r = self.m.lora_tx(payload)
                detail = (f"LoRa tx\n"
                          f"command  {line.strip()}\n"
                          f"sent     {len(payload)} bytes\n"
                          f"result   {self._od_status(r)}")
                return ("fire", "LoRa", f"tx {len(payload)}B", detail)
            # host-side recon analytics (no wire REQ — summarize received data)
            if radio == "wifi" and verb == "scan":
                rows = self.m.scan_wifi()
                body = "\n".join(f"  {r['mac']}  {self.src_name(r['src'])}  "
                                 f"{r['rssi']}dBm  x{r['count']}" for r in rows[:20])
                return ("info", f"wifi scan ({len(rows)})",
                        f"WiFi scan — {len(rows)} emitters seen\n{body or '  (none yet)'}")
            if radio == "wifi" and verb == "station-scan":
                rows = self.m.scan_stations()
                body = "\n".join(f"  {r['mac']}  {r['rssi']}dBm  x{r['count']}"
                                 for r in rows[:20])
                return ("info", f"stations ({len(rows)})",
                        f"Station scan — {len(rows)} seen\n{body or '  (none yet)'}")
            if radio == "wifi" and verb == "dupe-ssid":
                cand = self.m.detect_dupe_ssid()
                if not cand:
                    return ("info", "dupe-ssid (clean)",
                            "Duplicate-SSID / rogue check\nno SSID seen on 2+ BSSIDs")
                body = "\n".join(f"  \"{c['ssid']}\" on {len(c['bssids'])} BSSIDs:\n"
                                 + "\n".join(f"    {b}" for b in c['bssids']) for c in cand)
                return ("info", f"dupe-ssid ({len(cand)})",
                        f"Duplicate-SSID / rogue candidates\n{body}")
            if radio == "sigint" and verb == "wardrive":
                log = self.m.wardrive_log()
                body = "\n".join(f"  {e['mac']}  {e['lat']:.5f},{e['lon']:.5f}  "
                                 f"{e['rssi']}dBm" for e in log[-20:])
                return ("info", f"wardrive ({len(log)})",
                        f"Wardrive — {len(log)} GPS-tagged sightings\n{body or '  (need a GPS fix + captures)'}")
            if radio == "bt" and verb == "scan":
                rows = self.m.scan_bt()
                body = "\n".join(f"  {r['mac']}  {self.src_name(r['src'])}  "
                                 f"{r['rssi']}dBm  x{r['count']}" for r in rows[:20])
                return ("info", f"bt scan ({len(rows)})",
                        f"BLE scan — {len(rows)} advertisers seen (hub's own radio)\n"
                        f"{body or '  (none yet — the hub streams src-5 automatically)'}")
            if radio == "tools" and verb == "sysstate":
                s = self.m.system_state()
                bands = s.get("band_mask", 0)
                names = [n for n, b in (("2.4G", 1), ("5G", 2), ("BLE", 4)) if bands & b]
                detail = (
                    f"Hub system state\n"
                    f"uptime    {s['uptime_s']} s\n"
                    f"bridge    {'interactive' if s['interactive'] else 'monitor'}\n"
                    f"sd        {'mounted' if s['sd_mounted'] else 'DOWN'}  "
                    f"free {s['sd_free_mb']} MiB  used {s['sd_used_mb']} MiB\n"
                    f"log       {s['rows_staged']} staged, {s['rows_committed']} B on card, "
                    f"{s['files_rolled']} rolls, dropped q={s['dropped_queue']} sd={s['dropped_sd_down']}\n"
                    f"jobs      done {s['jobs_done']}, timeouts {s['job_timeouts']}, "
                    f"busy {s['job_busy']}, results written {s['res_written']}\n"
                    f"cfg       {'MODIFIED' if s['cfg_modified'] else 'STOCK'} "
                    f"({s['override_count']} overrides)  gps {['1Hz','5Hz'][s['gps_rate']]}  "
                    f"ble {'active' if s['ble_scan_mode'] else 'passive'}  "
                    f"relog {'timed' if s['relog_mode'] else 'once'}"
                    f"{'/' + str(s['relog_window_s']) + 's' if s['relog_mode'] == 0 else ''}\n"
                    f"bands     {', '.join(names) or 'none'}  dwell {s['dwell_ms']} ms\n"
                    f"fix       {'valid' if s['fix_valid'] else 'none'} "
                    f"{s['lat_e7']/1e7:.5f},{s['lon_e7']/1e7:.5f} sats {s['sats']}")
                return ("info", "sysstate", detail)
            if radio == "tools" and verb == "gps":
                g = self.m.gps()
                if not g:
                    return ("info", "gps", "GPS\nno fix received yet")
                return ("info", "gps",
                        f"GPS\nlat {g['lat']/1e7:.6f}\nlon {g['lon']/1e7:.6f}\n"
                        f"sats {g['sats']}\nfix {'valid' if g.get('fix_qual',0)>0 else 'acquiring'}")
        except Busy:
            return ("now", "BUSY — one-at-a-time; wait + retry")
        except (IndexError, ValueError) as e:
            return ("now", f"{radio} {verb}: bad args ({e})")
        except Exception as e:
            return ("now", f"error: {e}")
        return ("now", f"{radio} {verb}: not handled")

    def run_command_async(self, line):
        """Run a command-bar line OFF the UI thread. A capture/sniff/NFC job
        blocks for its whole duration (session.request waits on the RESP), which
        froze both TUIs for seconds. The model is already lock-guarded and the
        event reader already mutates it from another thread, so a worker adds no
        new race. One job at a time (the hub/session are one-at-a-time anyway).
        Multi-check 2026-11-01 fix. Returns False if a job is already running."""
        # Atomic claim: acquire(blocking=False) tests AND takes the gate in one
        # step, so two submits can never both pass (a bare check-then-set bool
        # could, if a second caller thread ever existed). _cmd_busy mirrors it
        # for readers (status/tests).
        if not self._cmd_gate.acquire(blocking=False):
            self.now = "busy — previous command still running"
            return False
        self._cmd_busy = True

        def _work():
            try:
                self.run_command(line)
            except Exception as e:          # never let a worker die silently
                self.now = f"error: {e}"
            finally:
                self._cmd_busy = False
                self._cmd_gate.release()

        try:
            threading.Thread(target=_work, daemon=True).start()
        except Exception as e:              # thread could not start: un-wedge
            self._cmd_busy = False
            self._cmd_gate.release()
            self.now = f"error: could not start command ({e})"
            return False
        return True

    def run_command(self, line):
        """Execute a command-bar line via parse_command and route the result to
        the stream/rows/NOW. Shared by both TUIs so behavior can't diverge."""
        kind, *rest = self.parse_command(line)
        if kind == "fire":
            self.fire_result(rest[0], rest[1], rest[2])
            self.now = "streaming…"
        elif kind == "info":
            self.add_command_result(rest[0], rest[1])
            self.now = "streaming…"
        else:  # "now"
            if rest and rest[0]:
                self.now = rest[0]

    def rows_list(self):
        with self.lock:
            return list(self.rows.values())

    def _keys(self):
        return list(self.rows.keys())

    def sel_index(self):
        """Resolve the identity-based selection to a current list index (for the
        renderers' highlight). Follows newest when `_follow`; else locates the
        sticky key; falls back to the last row if the key vanished."""
        keys = self._keys()
        if not keys:
            return -1
        if self._follow or self._sel_key not in keys:
            self._sel_key = keys[-1]          # newest (appended last)
            return len(keys) - 1
        return keys.index(self._sel_key)

    def selected(self):
        rl = self.rows_list()
        i = self.sel_index()
        return rl[i] if 0 <= i < len(rl) else None

    def move(self, d):
        """Arrow ↑/↓: DISENGAGE follow and step the sticky selection by identity."""
        keys = self._keys()
        if not keys:
            return
        self._follow = False
        if self._sel_key in keys:
            i = keys.index(self._sel_key)
        else:
            i = len(keys) - 1                 # was following; start from newest
        i = max(0, min(len(keys) - 1, i + d))
        self._sel_key = keys[i]

    def jump_newest(self):
        """Re-arm follow and snap to the most recent row."""
        self._follow = True
        keys = self._keys()
        self._sel_key = keys[-1] if keys else None

    # ---- status bar (REMIX redesign) --------------------------------------
    # Single source of truth for BOTH renderers (curses + Textual). Returns an
    # ordered list of StatusCell(text, state). state drives color:
    #   "ok"=green  "warn"=amber  "bad"=red  "idle"=neutral/unknown
    # Wire src_id: 1=C5(5G), 2=XS3(2.4G), 3=PICO, 4=HEAD (FROZEN in fh_frame.h).
    def _crc_sample(self, now, crc_cum):
        """Diff the cumulative codec crc counter into a per-second rate over the
        recent window. Called once per refresh under no lock (scalars only)."""
        dt = now - self._crc_prev_t
        if self._crc_prev_t == 0.0:
            # first sample: establish baseline, no rate yet
            self._crc_prev, self._crc_prev_t = crc_cum, now
            return 0.0
        if dt >= self.CRC_WIN_S:
            dcrc = max(0, crc_cum - self._crc_prev)
            self._crc_rate = dcrc / dt if dt > 0 else 0.0
            self._crc_prev, self._crc_prev_t = crc_cum, now
        return self._crc_rate

    def status_bar(self):
        now = time.monotonic()
        attached = bool(getattr(self.m, "attached", False)) and not getattr(self.m.s, "link_lost", False)
        codec = self.m.s.codec
        crc_rate = self._crc_sample(now, codec.crc_errors)
        with self.lock:
            last_good = self._last_good_t
            last_seen = dict(self._last_seen)
            gps = self._last_gps
            last_gps_t = self._last_gps_t

        def fresh(t):      # seen within the liveness window?
            return t > 0.0 and (now - t) <= self.FRESH_S

        cells = []
        # 1) Medusa — hub link: attached AND frames flowing (liveness)
        flowing = fresh(last_good)
        cells.append(StatusCell("Medusa", "ok" if (attached and flowing) else "bad"))

        # 2) Display — head (src_id 4): hub-confirmed bidirectional liveness.
        #    Medusa reads it as node-4 freshness (hub relays head PONG/HB as
        #    src_id 4). RED until head firmware answers — honest.
        cells.append(StatusCell("Display", "ok" if fresh(last_seen.get(4, 0.0)) else "bad"))

        # 3) X-S3 (2.4) — src_id 2 ; 4) X-C5 (5) — src_id 1
        cells.append(StatusCell("X-S3", "ok" if fresh(last_seen.get(2, 0.0)) else "bad"))
        cells.append(StatusCell("X-C5", "ok" if fresh(last_seen.get(1, 0.0)) else "bad"))

        # 5) GPS — three-tier. fresh frame + fix_qual>0 => green(+sats);
        #    fresh frame + fix_qual==0 => amber (acquiring); no frame => red.
        gps_fresh = last_gps_t > 0.0 and (now - last_gps_t) <= self.GPS_FRESH_S
        if gps_fresh and gps and gps.get("fix_qual", 0) > 0:
            cells.append(StatusCell(f"GPS●{gps.get('sats', 0)}", "ok"))
        elif gps_fresh:
            cells.append(StatusCell("GPS", "warn"))
        else:
            cells.append(StatusCell("GPS", "bad"))

        # 6) link — USB integrity only. Quiet/green when clean; loud when crc
        #    rising in the recent window. Silence is NOT corruption -> stays ok.
        if crc_rate > 0.0:
            cells.append(StatusCell(f"link ⚠ {crc_rate:.0f} crc/s", "bad"))
        else:
            cells.append(StatusCell("link", "ok"))
        return cells


# ============================================================ VIEW (curses)
def run_view(stdscr, model: MedusaModel, use_sim: bool):
    import curses
    curses.curs_set(0)
    stdscr.nodelay(True)
    # status-bar color pairs (ok/warn/bad) + stream palette; degrade if no color.
    HAS_COLOR = False
    try:
        if curses.has_colors():
            curses.start_color()
            curses.use_default_colors()
            curses.init_pair(1, curses.COLOR_GREEN, -1)   # ok / 5GHz dark green
            curses.init_pair(2, curses.COLOR_YELLOW, -1)  # warn / GPS yellow
            curses.init_pair(3, curses.COLOR_RED, -1)     # bad
            curses.init_pair(4, curses.COLOR_BLUE, -1)    # BT
            curses.init_pair(5, curses.COLOR_WHITE, -1)   # SUM / default
            # orange: color 208 on 256-color terminals, else yellow+bold approx.
            try:
                curses.init_pair(6, 208 if curses.COLORS >= 256 else curses.COLOR_YELLOW, -1)
            except Exception:
                curses.init_pair(6, curses.COLOR_YELLOW, -1)
            HAS_COLOR = True
    except Exception:
        HAS_COLOR = False
    # stream color token -> curses attr. green_light = green+BOLD (brighter),
    # green_dark = plain green. Orange bold when it fell back to yellow.
    def _sc(token):
        if not HAS_COLOR:
            return curses.A_NORMAL
        orange_extra = 0 if (curses.COLORS or 0) >= 256 else curses.A_BOLD
        return {
            "blue":        curses.color_pair(4),
            "green_light": curses.color_pair(1) | curses.A_BOLD,
            "green_dark":  curses.color_pair(1),
            "yellow":      curses.color_pair(2),
            "white":       curses.color_pair(5),
            "orange":      curses.color_pair(6) | orange_extra,
        }.get(token, curses.color_pair(5))
    STATE_ATTR = {
        "ok":   (curses.color_pair(1) if HAS_COLOR else curses.A_NORMAL),
        "warn": (curses.color_pair(2) | curses.A_BOLD) if HAS_COLOR else curses.A_BOLD,
        "bad":  (curses.color_pair(3) | curses.A_BOLD) if HAS_COLOR else curses.A_REVERSE,
        "idle": curses.A_DIM,
    }
    model.m.connect(use_sim=use_sim)
    mode = {"overlay": None, "cmd": ""}      # overlay: None | 'cmd' | 'menu'
    MENU = ["WiFi", "BLE", "2.4GHz (nRF24)", "NFC (PN532)", "SubGHz (CC1101)",
            "LoRa / stick", "GPS", "Tools", "Settings"]
    menu_sel = {"i": 0}

    def draw():
        stdscr.erase()
        h, w = stdscr.getmaxyx()
        # STATUS BAR (always, expanded) — per-cell color-coded (REMIX redesign).
        # Group separators sit between {Medusa}|{peers}|{GPS}|{link}.
        stdscr.addnstr(0, 0, " ".ljust(w), w, curses.A_REVERSE)   # base bar
        cells = model.status_bar()
        # group boundaries: after cell 0 (Medusa) and 3 (peers) and 4 (GPS)
        seps_after = {0, 3, 4}
        x = 1
        for i, cell in enumerate(cells):
            if x >= w - 1:
                break
            attr = STATE_ATTR.get(cell.state, curses.A_NORMAL) | curses.A_REVERSE
            stdscr.addnstr(0, x, cell.text, max(0, w - 1 - x), attr)
            x += len(cell.text) + 2
            if i in seps_after and x < w - 2:
                stdscr.addnstr(0, x, "│", 1, curses.A_REVERSE)
                x += 2
        # LIVE STREAM strip (read-only, newest last) — proportional (~1/3 of the
        # usable height, never below 5). Rows 1..stream_h.
        stream_h = max(5, (h - 3) // 3)      # h minus status(1)+header(1)+now(1)
        with model.lock:
            tail = model.stream[-stream_h:]
        for i, (ln, color) in enumerate(tail):
            stdscr.addnstr(1 + i, 0, ln[:w], w, _sc(color))
        # columns — header row + first content row derived from the stream height
        hdr_row = 1 + stream_h
        top = hdr_row + 1
        colw = w // 2
        bottom = h - 2
        rl = model.rows_list()
        if mode["overlay"] == "menu":
            stdscr.addnstr(hdr_row, 0, " MENU (m) — ↑↓ select · esc close ".ljust(colw), colw, curses.A_BOLD)
            for i, item in enumerate(MENU):
                y = top + i
                if y >= bottom: break
                mark = "▸" if i == menu_sel["i"] else " "
                stdscr.addnstr(y, 0, f"{mark} {item}"[:colw], colw,
                               curses.A_REVERSE if i == menu_sel["i"] else 0)
        else:
            follow = " ▶following" if model._follow else ""
            stdscr.addnstr(hdr_row, 0, f" DEVICES / EVENTS (↑↓ select · End=newest){follow} ".ljust(colw), colw, curses.A_BOLD)
            model.sel_index()               # resolve/refresh the sticky key
            sel_key = model._sel_key
            for i, r in enumerate(rl):
                y = top + i
                if y >= bottom: break
                star = "★" if r.star else " "
                line = f"{star}{r.label}  x{r.seen}"
                stdscr.addnstr(y, 0, line[:colw], colw,
                               curses.A_REVERSE if r.key == sel_key else 0)
        # RIGHT detail
        stdscr.addnstr(hdr_row, colw + 1, " DETAIL ".ljust(w - colw - 1), w - colw - 1, curses.A_BOLD)
        sel = model.selected()
        if mode["overlay"] == "menu":
            detail = f"{MENU[menu_sel['i']]}\n\n(preview — ⏎ would open this radio's actions)"
        else:
            detail = (sel.detail if sel else "(no selection)")
        for i, ln in enumerate(detail.split("\n")):
            y = top + i
            if y >= bottom: break
            stdscr.addnstr(y, colw + 1, ln[:w - colw - 1], w - colw - 1)
        # NOW line / command bar
        if mode["overlay"] == "cmd":
            stdscr.addnstr(h - 1, 0, (": " + mode["cmd"]).ljust(w), w)
        else:
            hint = "  c command · m menu · q quit"
            stdscr.addnstr(h - 1, 0, (" NOW: " + model.now + hint)[:w].ljust(w), w, curses.A_REVERSE)
        stdscr.refresh()

    def run_command(line):
        # All parsing/dispatch lives in the shared model (verb grammar, human
        # units, reserved-verb handling) so both TUIs behave identically.
        if line.strip():
            model.now = f"running {line.split()[0].lower()}…"
            model.run_command_async(line)   # off-thread: UI keeps redrawing

    # main loop
    while True:
        draw()
        try:
            ch = stdscr.getch()
        except Exception:
            ch = -1
        if ch == -1:
            time.sleep(0.05); continue
        ov = mode["overlay"]
        if ov == "cmd":
            if ch in (10, 13):
                run_command(mode["cmd"].strip()); mode["cmd"] = ""; mode["overlay"] = None
            elif ch == 27:   # esc
                mode["overlay"] = None; mode["cmd"] = ""
            elif ch in (curses.KEY_BACKSPACE, 127, 8):
                mode["cmd"] = mode["cmd"][:-1]
            elif 32 <= ch < 127:
                mode["cmd"] += chr(ch)
        elif ov == "menu":
            if ch == 27:
                mode["overlay"] = None
            elif ch == curses.KEY_UP:
                menu_sel["i"] = max(0, menu_sel["i"] - 1)
            elif ch == curses.KEY_DOWN:
                menu_sel["i"] = min(len(MENU) - 1, menu_sel["i"] + 1)
        else:
            if ch == ord('q'):
                break
            elif ch == curses.KEY_UP:
                model.move(-1)
            elif ch == curses.KEY_DOWN:
                model.move(1)
            elif ch in (curses.KEY_END, ord('G')):   # jump to newest, re-arm follow
                model.jump_newest()
            elif ch == 3:    # ctrl/alt+C varies by terminal; also accept 'c'
                mode["overlay"] = "cmd"
            elif ch in (ord('c'),):
                mode["overlay"] = "cmd"
            elif ch in (ord('m'),):
                mode["overlay"] = "menu"
    model.m.close()


# ============================================================ entry
def main():
    args = sys.argv[1:]
    if "--selftest-model" in args:
        _selftest_model(); return
    use_sim = "--sim" in args
    args = [a for a in args if a != "--sim"]
    if not use_sim and not args:
        print(__doc__); sys.exit(1)
    port = args[0] if args else "sim"
    import curses
    model = MedusaModel(MedusaClient(port))
    curses.wrapper(run_view, model, use_sim)


def _selftest_model():
    """Headless smoke test of the MODEL (no curses, no terminal needed)."""
    m = MedusaClient(port="sim")
    model = MedusaModel(m)
    m.connect(use_sim=True)
    time.sleep(2.0)                 # let the sim stream populate rows
    rl = model.rows_list()
    assert len(rl) > 0, "no rows populated from the stream"
    assert len(model.stream) > 0, "live stream empty"
    # --- left-pane selection (identity-sticky + follow-newest) --------------
    # default follows newest
    assert model._follow, "should follow newest by default"
    newest_key = list(model.rows.keys())[-1]
    assert model._sel_key == newest_key or model.sel_index() == len(model.rows)-1
    # arrow up: disengages follow, cursor sticks to an item by identity
    model.move(-1)
    assert not model._follow, "↑ should disengage follow"
    pinned = model._sel_key
    # new rows arriving must NOT slide the highlight off the pinned item
    model._touch_row("zz:new:dev:00", "device", "zz new", "d")
    assert model._sel_key == pinned, "sticky selection slid off on new row"
    # jump-to-newest re-arms follow and snaps to the last row
    model.jump_newest()
    assert model._follow and model._sel_key == list(model.rows.keys())[-1]
    # a commanded row auto-selects + pins (focus follows intent)
    model.add_command_result("CC1101 433920 1s", "test detail")
    assert model.selected().star and model.selected().label.startswith("CC1101")
    assert not model._follow, "firing a command should pin (stop following)"
    # status bar (REMIX redesign): the sim sends src 1/2/3 + GPS(fix_qual=1),
    # but NO src 4 (head) -> Display must be red, C5/X-S3 green, GPS green.
    bar = model.status_bar()
    assert len(bar) == 6, f"expected 6 status cells, got {len(bar)}"
    labels = [c.text for c in bar]
    disp = next(c for c in bar if c.text == "Display")
    xs3  = next(c for c in bar if c.text == "X-S3")
    xc5  = next(c for c in bar if c.text == "X-C5")
    gps  = next(c for c in bar if c.text.startswith("GPS"))
    assert disp.state == "bad", f"Display should be red (no head src4); got {disp.state}"
    assert xs3.state == "ok",  f"X-S3 should be green (src2 beating); got {xs3.state}"
    assert xc5.state == "ok",  f"X-C5 should be green (src1 beating); got {xc5.state}"
    assert gps.state == "ok" and "●" in gps.text, f"GPS should be green w/ sats; got {gps.text}/{gps.state}"
    # stream naming: BT present (src 5), shortened verbs, and a FIRED blade op
    # renders by radio name with a per-action summary — all before close.
    assert any("BT" in t for t, _ in model.stream), "no BT (src 5) line in the stream"
    assert any("CAP " in t for t, _ in model.stream), "no shortened CAP verb in the stream"
    model.run_command("cc1101 cap 433920 3sec")   # real path → clean detail
    assert any("CAP CC1101 — 3 SEC" in t for t, _ in model.stream), \
        "fired CC1101 op did not render 'CAP CC1101 — 3 SEC' in the stream"
    assert not any("pico" in t for t, _ in model.stream), "'pico' leaked into the stream"
    # colors (REMIX palette): BT=blue, GPS=yellow, fired op=orange, 2.4=green_light
    assert any(c == "blue"   for t, c in model.stream if "BT" in t),  "BT not blue"
    assert any(c == "yellow" for t, c in model.stream if t.startswith("GPS")), "GPS not yellow"
    assert any(c == "orange" for t, c in model.stream if "CC1101 — 3 SEC" in t), "fired op not orange"
    assert any(c == "green_light" for t, c in model.stream if "x-s3(2.4)" in t), "2.4 not green_light"
    # --- CC1101 replay-from (decode-triple store) ---------------------------
    # A successful capture leaves its triple in the store; `cc1101 captures`
    # lists it, and `cc1101 replay <freq> <seq>` re-transmits that exact triple
    # via the implemented 0x34 ext-band TX verb (blade-side last-capture NOT
    # required — the stored values are sent from Medusa).
    seqs = sorted(m._cc_captures)
    assert seqs, "capture triple store empty after a successful capture"
    seq0 = seqs[0]
    cap = m._cc_captures[seq0]
    k = model.parse_command(f"cc1101 replay {cap['freq_khz']} {seq0}")
    assert k[0] == "fire" and k[2] == "REPLAY", f"replay-from should fire: {k}"
    assert "replay-from" in k[3], f"replay-from detail wrong: {k[3]}"
    assert "result   FH_OD_OK" in k[3], f"replay-from must echo OK: {k[3]}"
    assert f"proto={cap['proto']} bits={cap['bits']} value={cap['value']}" in k[3], \
        f"replay-from triple mismatch: {k[3]}"
    k = model.parse_command("cc1101 captures")
    assert k[0] == "info" and "captures" in k[1], f"captures verb failed: {k}"
    assert f"seq={seq0:>4}" in k[2] or f"seq={seq0}" in k[2], \
        f"captures listing missing the capture: {k[2]}"
    k = model.parse_command("cc1101 replay 433920 999")
    assert k[0] == "now" and "no stored capture" in k[1], f"unknown seq: {k}"
    # --- command-bar verb grammar -----------------------------------------
    assert MedusaModel._dur_ms("11sec", 0) == 11000, "11sec not parsed"
    assert MedusaModel._dur_ms("500ms", 0) == 500, "500ms not parsed"
    assert MedusaModel._dur_ms("2s", 0) == 2000, "2s not parsed"
    k = model.parse_command("cc1101 cap 433920 11sec")
    assert k[0] == "fire" and k[1] == "CC1101" and k[2] == "11 SEC", f"cap grammar: {k}"
    k = model.parse_command("cc1101 replay 433920")
    assert k[0] == "fire" and k[1] == "CC1101" and k[2] == "REPLAY", f"replay should now be live-fire: {k}"
    k = model.parse_command("cc1101 raw 433920 3s")
    assert k[0] == "fire" and k[1] == "CC1101" and k[2] == "RAW", f"raw should be live-fire: {k}"
    k = model.parse_command("nrf24 sniff 76 5s")
    assert k[0] == "fire" and k[1] == "nRF24" and k[2] == "SNIFF", f"sniff should be live-fire: {k}"
    k = model.parse_command("nrf24 scan 0 80")
    assert k[0] == "fire" and k[1] == "nRF24", f"scan grammar: {k}"
    k = model.parse_command("bogus foo")
    assert k[0] == "now" and "unknown module" in k[1], f"unknown module: {k}"
    k = model.parse_command("cc1101 nonsense")
    assert k[0] == "now" and "unknown verb" in k[1], f"unknown verb: {k}"
    # HALEHOUND surface: capture/recon verbs recognized (reserved), transmit-
    # disruption verbs recognized as external-build (extern) — all PRESENT.
    k = model.parse_command("sigint eapol")
    assert k[0] == "now" and "capture/recon" in k[1], f"eapol should be reserved-recon: {k}"
    k = model.parse_command("sigint wardrive")
    assert k[0] == "info" and "wardrive" in k[1], f"wardrive should be live info: {k}"
    k = model.parse_command("wifi scan")
    assert k[0] == "info" and "scan" in k[1], f"wifi scan should be live info: {k}"
    k = model.parse_command("wifi dupe-ssid")
    assert k[0] == "info" and "dupe-ssid" in k[1], f"dupe-ssid should be live info: {k}"
    k = model.parse_command("bt scan")
    assert k[0] == "info" and "BLE scan" in k[2], f"bt scan should be live info: {k}"
    assert m.scan_bt() and all(r["src"] == 5 for r in m.scan_bt()), \
        "scan_bt must return only src-5 (BLE) rows"
    k = model.parse_command("tools sysstate")
    assert k[0] == "info" and k[1] == "sysstate" and "uptime" in k[2], f"sysstate: {k}"
    # exact SSID-twin detection: sim advertises "HomeNet" on two BSSIDs
    cand = m.detect_dupe_ssid()
    assert any(c["ssid"] == "HomeNet" and len(c["bssids"]) >= 2 for c in cand), \
        f"should detect HomeNet twin on 2 BSSIDs: {cand}"
    k = model.parse_command("tools gps")
    assert k[0] == "info" and k[1] == "gps", f"tools gps should be live info: {k}"
    k = model.parse_command("wifi deauth")
    assert k[0] == "now" and "external module" in k[1], f"deauth should be extern: {k}"
    k = model.parse_command("wifi deauth-watch")
    assert k[0] == "now" and "capture/recon" in k[1], f"deauth-watch reserved-recon: {k}"
    k = model.parse_command("bt beacon")
    assert k[0] == "now" and "external module" in k[1], f"bt beacon should be extern: {k}"
    # Tier 3 extern band: blade externs are WIRED (command-line only, no menu).
    # No authored implementation exists yet, so the blade answers EXT_STUB —
    # proving the routing works end to end.
    k = model.parse_command("cc1101 jam 433920 0 3000")
    assert k[0] == "fire" and k[2] == "STUB" and "STUB" in k[3], f"cc1101 jam should fire as extern STUB: {k}"
    k = model.parse_command("nrf24 protokill 76")
    assert k[0] == "fire" and k[2] == "STUB", f"nrf24 protokill should fire as extern STUB: {k}"
    # Tier 3 TX verbs (IMPLEMENTED in the blades + simulator): full round-trip
    # through the simulator — REQ → RESP → clean detail with the echo.
    k = model.parse_command("cc1101 tx 433920 1 24 0x2A2B3C")
    assert k[0] == "fire" and k[1] == "CC1101" and k[2] == "TX", f"cc1101 tx should be live-fire: {k}"
    assert "result   FH_OD_OK" in k[3], f"cc1101 tx must echo OK: {k[3]}"
    assert "proto=1 bits=24 value=2763580" in k[3], f"cc1101 tx echo wrong: {k[3]}"
    k = model.parse_command("nrf24 tx 76 0x1A2B3C")
    assert k[0] == "fire" and k[1] == "nRF24" and k[2] == "TX", f"nrf24 tx should be live-fire: {k}"
    assert "payload 1A2B3C (3 B)" in k[3], f"nrf24 tx detail wrong: {k[3]}"
    assert "writes acknowledged: 1/1" in k[3], f"nrf24 tx ack count wrong: {k[3]}"
    k = model.parse_command("cc1101 tx 433920 1 40 0x1")
    assert k[0] == "now" and "bits" in k[1], f"cc1101 tx must reject bad bits: {k}"
    # --- DETAIL records: clean metadata, command-echo, NO struct dump --------
    k = model.parse_command("cc1101 cap 433920 15sec")
    assert "command  cc1101 cap 433920 15sec" in k[3], f"fired-op must echo command: {k[3]}"
    assert "recorded 15s" in k[3], f"fired-op must state recorded duration: {k[3]}"
    assert "{'" not in k[3] and "seq'" not in k[3], f"struct dump leaked into detail: {k[3]}"
    wifi = [r for r in model.rows_list() if r.kind == "device" and "Wi-Fi" in r.detail]
    assert wifi and "frame  " in wifi[0].detail, "Wi-Fi detail missing frame-type"
    assert not any("{'" in r.detail for r in model.rows_list()), "a row detail still dumps a struct"
    m.close()
    print(f"MODEL OK — {len(rl)} rows, {len(model.stream)} stream lines, "
          f"commanded row auto-selected ✓")
    print(f"STATUS BAR OK — {labels}")
    print("STREAM NAMES OK — BT present, CAP/SUM verbs, fired blade=CAP CC1101 — 3 SEC, no 'pico'")


if __name__ == "__main__":
    main()
