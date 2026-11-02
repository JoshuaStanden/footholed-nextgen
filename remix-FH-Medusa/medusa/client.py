"""
medusa.client — the high-level bench API (the thing the TUI, or your scripts,
talk to). Verbs map 1:1 to fh_ondemand.h; events map to fh_frame.h up-frames.

    m = MedusaClient("/dev/tty.usbmodemXXXX"); m.connect()
    m.on_gps(lambda fix: print(fix))
    m.on_capture(lambda cap: print(cap))
    print(m.ping())                      # readiness + which radios init'd
    print(m.gps())                       # latest fix (poll)
    print(m.sample_nrf24(0, 80, dwell_ms=200))   # "15s of nRF24" style job
    print(m.sample_cc1101(433920, dwell_ms=15000))

Everything is serialized by the BUSY lock in session.py — a second request while
one is running raises medusa.session.Busy.
"""
from __future__ import annotations
import time
from dataclasses import dataclass
from .protocol import load, Protocol
from .session import Session, Busy, Timeout

# host-bridge control codes (match fh_hub_bridge.h; 0x70-0x73 free band)
FH_BR_ATTACH, FH_BR_DETACH, FH_BR_ATTACH_ACK, FH_BR_HOST_HB = 0x70, 0x71, 0x72, 0x73


@dataclass
class Readiness:
    status: str
    radios_ready: dict      # {'CC1101': True, 'nRF24': False, ...}


class MedusaClient:
    def __init__(self, port: str, baud: int = 921600):
        self.p: Protocol = load()                 # drift-guarded protocol
        self.s = Session(self.p, port, baud)
        self.s.on_event(self._dispatch_event)
        self.s.on_event(self._detect_ack)
        self.attached = False            # set True once the hub ACKs ATTACH
        import threading as _th
        self._ack_evt = _th.Event()
        # user event callbacks
        self._cbs = {k: [] for k in
                     ("capture", "gps", "summary", "heartbeat", "lora_rx", "ssid")}
        # live node liveness, keyed by src_id (from heartbeats)
        self.nodes: dict[int, dict] = {}
        # OBSERVATION STORE (host-side recon analytics over received captures —
        # feeds scan/station-scan/wardrive/dupe-ssid library verbs; no wire
        # change, these analyze frames we already receive). Keyed by MAC.
        self._obs: dict[str, dict] = {}
        self._obs_max = 2048
        self._wardrive: list = []          # (mac, src, rssi, lat, lon, ts) sightings
        self._wardrive_max = 4096
        self._ssid: dict[str, str] = {}    # MAC(BSSID) -> decoded SSID ("" = hidden)
        self._results: list = []           # job-results log (the 'results' verb)
        self._results_max = 64
        # CC1101 DECODE-TRIPLE STORE: every successful capture RESP (status OK,
        # data[0]=proto, data[1]=bits, data[2..5]=value LE) is remembered here
        # keyed by its seq, so `cc1101 replay <freq> <seq>` can re-transmit a
        # SPECIFIC older capture (not just the blade's last one) by firing the
        # implemented 0x34 ext-band TX verb with the stored triple. Bounded.
        self._cc_captures: dict[int, dict] = {}    # seq -> capture triple
        self._cc_captures_max = 32
        # CAPTURE-TO-DUMP STORE (Tier 2): chunk streams from the blades'
        # capture-to-dump jobs (FH_OD_DUMP_IND, 0x2F) reassemble here, keyed by
        # the job's seq. `self._dumps` holds FINALIZED dumps (bounded, oldest
        # dropped); records are parsed per the radio tag's layout.
        self._dumps: dict[int, dict] = {}      # seq -> finalized dump
        self._dumps_active: dict[int, dict] = {}  # seq -> in-progress stream
        self._dumps_max = 32

    def connect(self, use_sim: bool = False):
        # reconnect-safe (CLEAN-3): tear down any prior connection first so a
        # second connect() (e.g. after a cable replug) never leaks the old
        # reader + heartbeat threads or leaves two readers racing on _route.
        if getattr(self, "_connected", False):
            try:
                self.close()
            except Exception:
                pass
        if use_sim:
            from .simulator import SimHub
            self.s._ser = SimHub()                 # drop-in for the serial port
            self.s._stop.clear()
            self.s.link_lost = False               # parity with Session.connect()
            import threading
            self.s._rx_thread = threading.Thread(target=self.s._reader, daemon=True)
            self.s._rx_thread.start()
        else:
            self.s.connect()
        # host-bridge handshake: tell the hub to enter INTERACTIVE mode, then
        # heartbeat so it stays there (it reverts to monitor on silence). The
        # simulator has no bridge and simply ignores these control frames.
        import threading
        self._bridge_stop = False
        self._bridge_stop_evt = threading.Event()
        self._ack_evt.clear(); self.attached = False
        try:
            self.s.send_fire_and_forget(FH_BR_ATTACH, b"\x00")
        except Exception:
            pass
        self._hb_thread = threading.Thread(target=self._bridge_heartbeat, daemon=True)
        self._hb_thread.start()
        # wait briefly for the hub's ATTACH_ACK. On a real hub this confirms we
        # went interactive; the simulator never ACKs, so attached stays False but
        # the stream still flows (the TUI shows 'attached' vs 'no ack' accordingly).
        self._ack_evt.wait(1.5)
        self._connected = True

    def _bridge_heartbeat(self):
        # Event-based wait so close() wakes us immediately (MULDER-9): no lingering
        # 2 s sleep writing to a closing port.
        while not self._bridge_stop:
            if getattr(self.s, "link_lost", False):
                # reader thread gave up (port truly gone) — don't keep claiming
                # ATTACH; drop it so the UI goes red instead of staying stuck green.
                self.attached = False
                break
            try:
                self.s.send_fire_and_forget(FH_BR_HOST_HB, b"\x00")
            except Exception:
                pass
            if self._bridge_stop_evt.wait(2.0):    # < FH_BR_ATTACH_TIMEOUT (5s)
                break

    def close(self):
        self._connected = False
        self._bridge_stop = True
        evt = getattr(self, "_bridge_stop_evt", None)
        if evt is not None:
            evt.set()                              # wake the heartbeat NOW
        hb = getattr(self, "_hb_thread", None)
        if hb is not None:
            hb.join(timeout=1.0)                   # let it exit before we close the port
        try:
            self.s.send_fire_and_forget(FH_BR_DETACH, b"\x00")
        except Exception:
            pass
        self.s.close()

    # ---- event registration -------------------------------------------------
    def on_capture(self, cb):   self._cbs["capture"].append(cb)
    def on_gps(self, cb):       self._cbs["gps"].append(cb)
    def on_summary(self, cb):   self._cbs["summary"].append(cb)
    def on_heartbeat(self, cb): self._cbs["heartbeat"].append(cb)
    def on_lora_rx(self, cb):   self._cbs["lora_rx"].append(cb)
    def on_ssid(self, cb):      self._cbs["ssid"].append(cb)

    def _emit(self, kind, obj):
        for cb in self._cbs[kind]:
            try: cb(obj)
            except Exception: pass

    def _detect_ack(self, frame):
        if frame.type == FH_BR_ATTACH_ACK:
            self.attached = True
            self._ack_evt.set()

    def _dispatch_event(self, frame):
        t = self.p.type_name.get(frame.type, hex(frame.type))
        if t == "FH_TYPE_CAPTURE":
            cap = self.p.s("fh_capture_t").unpack(frame.payload)
            self._observe(cap)
            self._emit("capture", cap)
        elif t == "FH_TYPE_GPS_FIX":
            fix = self.p.s("fh_gps_fix_t").unpack(frame.payload)
            self._last_gps = fix
            self._emit("gps", fix)
        elif t == "FH_TYPE_SUMMARY":
            self._emit("summary", self.p.s("fh_summary_t").unpack(frame.payload))
        elif t == "FH_TYPE_SSID":
            s = self.p.s("fh_ssid_t").unpack(frame.payload)
            self._observe_ssid(s)
            self._emit("ssid", s)
        elif t == "FH_TYPE_HEARTBEAT":
            hb = self.p.s("fh_heartbeat_t").unpack(frame.payload)
            self.nodes[hb["src_id"]] = hb
            self._emit("heartbeat", hb)
        elif t == "FH_OD_LORA_RX_IND":
            self._emit("lora_rx", self.p.s("fh_od_lora_rx_ind_t").unpack(frame.payload))
        elif t == "FH_OD_DUMP_IND":
            self._on_dump_ind(self.p.s("fh_od_dump_ind_t").unpack(frame.payload))

    # ---- helpers ------------------------------------------------------------
    def _status(self, code: int) -> str:
        return self.p.od_status.get(code, f"0x{code:02X}")

    def _radios(self, bitmap: int) -> dict:
        d = self.p.defines
        return {"CC1101": bool(bitmap & d["FH_OD_RADIO_CC1101"]),
                "nRF24":  bool(bitmap & d["FH_OD_RADIO_NRF24"]),
                "PN532":  bool(bitmap & d["FH_OD_RADIO_PN532"]),
                "SX1262": bool(bitmap & d["FH_OD_RADIO_SX1262"])}

    def _req(self, req_name, payload, resp_name, timeout=2.0):
        t0 = time.time()
        try:
            rf = self.s.request(self.p.type_by_name[req_name], payload,
                                self.p.type_by_name[resp_name], timeout)
        except Busy as b:
            self._log_result(req_name, "BUSY", "slot busy (one-at-a-time)", t0)
            raise
        except Timeout as t:
            self._log_result(req_name, "TIMEOUT", f"no RESP in {timeout}s", t0)
            raise
        out = self.p.s(resp_name_to_struct(resp_name)).unpack(rf.payload)
        self._log_result(req_name, self._status(out.get("status")), out, t0)
        return out

    # ---- job-results log (bench view of the hub's on-demand jobs) ----------
    # Every fired job (ping/gps/sample_*/read_nfc/lora_tx) is logged here with
    # its outcome — including BUSY and TIMEOUT — so the bench can see at a
    # glance what the hub's requester did, matching the rows it writes to
    # /sdcard/logs/results.csv on the SD card. Bounded (oldest dropped).
    def _log_result(self, req_name, status, detail, t0):
        import time as _t
        if len(self._results) >= self._results_max:
            self._results.pop(0)
        rec = {"t": _t.time(), "req": req_name.replace("FH_OD_", "").replace("_REQ", "").lower(),
               "status": status, "elapsed": _t.time() - t0}
        if isinstance(detail, dict):
            if detail.get("rssi") is not None:
                rec["rssi"] = detail["rssi"]
            if detail.get("active_chan") not in (None, 0xFF):
                rec["chan"] = detail["active_chan"]
            if "toa_ms" in detail:
                rec["toa_ms"] = detail["toa_ms"]
            if "uid" in detail:
                rec["uid"] = bytes(detail["uid"])[: detail.get("uid_len", 0)].hex()
            if detail.get("len") is not None:
                rec["cap_len"] = detail["len"]
        self._results.append(rec)

    def results(self) -> list:
        """Recent on-demand job results, oldest-first. The 'results' verb."""
        return list(self._results)

    # ---- host-side recon analytics (over received captures; no wire I/O) ----
    # These are REAL library verbs computed from the observation store. They do
    # NOT send a REQ — they summarize what the sniffers already streamed in. The
    # TUI command bar maps wifi scan / station-scan / dupe-ssid / wardrive to
    # these. (Firmware-dependent verbs — monitor/spectrum/deauth-watch/bt-*/
    # eapol — still need the hub to emit those frame kinds; not these.)
    def _observe(self, cap: dict):
        import time as _t
        mac = bytes(cap["mac"]).hex(":")
        rec = self._obs.get(mac)
        if rec is None:
            if len(self._obs) >= self._obs_max:          # bounded; drop oldest
                self._obs.pop(next(iter(self._obs)))
            rec = {"mac": mac, "src": cap["src_id"], "first": _t.time(),
                   "count": 0, "rssi": cap["rssi"], "rssi_max": cap["rssi"],
                   "band_chan": cap["band_chan"], "frame_type": cap["frame_type"]}
            self._obs[mac] = rec
        rec["count"] += 1
        rec["last"] = _t.time()
        rec["rssi"] = cap["rssi"]
        rec["rssi_max"] = max(rec["rssi_max"], cap["rssi"])
        rec["frame_type"] = cap["frame_type"]
        # wardrive sighting: tag with the latest GPS fix we hold (if any)
        g = getattr(self, "_last_gps", None)
        if g and g.get("fix_qual", 0) > 0:
            if len(self._wardrive) >= self._wardrive_max:
                self._wardrive.pop(0)
            self._wardrive.append({"mac": mac, "src": cap["src_id"],
                                   "rssi": cap["rssi"], "lat": g["lat"] / 1e7,
                                   "lon": g["lon"] / 1e7, "ts": _t.time()})

    def scan_wifi(self) -> list:
        """Observed Wi-Fi emitters (src 1/2), newest-first. Real summary of the
        capture stream — the 'scan' verb's result."""
        rows = [r for r in self._obs.values() if r["src"] in (1, 2)]
        return sorted(rows, key=lambda r: r.get("last", 0), reverse=True)

    def scan_stations(self) -> list:
        """Observed client/station MACs (same store; a station vs AP split needs
        richer 802.11 fields in `extra`, not present yet — so this returns all
        observed emitters and is honest about that)."""
        return self.scan_wifi()

    def scan_bt(self) -> list:
        """Observed BLE advertisers (src 5 = the hub's own radio), newest-first.
        The hub already streams src-5 captures into the SAME observation store
        as Wi-Fi (bench-proven 2026-10-30: BLE with maker/service decode); this
        is the missing filter over data we already hold — no wire I/O. Rows
        carry the BLE extras where available (adv channel, company id)."""
        rows = [r for r in self._obs.values() if r["src"] == 5]
        return sorted(rows, key=lambda r: r.get("last", 0), reverse=True)

    def detect_dupe_ssid(self) -> list:
        """Duplicate-SSID / rogue-AP detector. Now EXACT: with the SSID frame
        (FH_TYPE_SSID) the host holds MAC(BSSID)->SSID, so it flags any SSID
        advertised by TWO OR MORE distinct BSSIDs — the classic evil-twin /
        duplicate-network condition a router maker wants to detect. Returns one
        dict per duplicated SSID with the list of BSSIDs claiming it. Hidden
        SSIDs ("") are ignored. Empty when no SSID is claimed by >1 BSSID."""
        by_ssid: dict[str, list] = {}
        for mac, ssid in self._ssid.items():
            if ssid:                              # skip hidden ("")
                by_ssid.setdefault(ssid, []).append(mac)
        out = []
        for ssid, macs in by_ssid.items():
            if len(macs) >= 2:
                out.append({"ssid": ssid, "bssids": sorted(macs),
                            "note": "same SSID on multiple BSSIDs — "
                                    "duplicate/rogue candidate"})
        return out

    def _observe_ssid(self, s: dict):
        mac = bytes(s["mac"]).hex(":")
        n = min(int(s.get("ssid_len", 0)), 32)
        raw = bytes(s.get("ssid", b""))[:n]
        try:
            self._ssid[mac] = raw.decode("utf-8", "replace")
        except Exception:
            self._ssid[mac] = raw.hex()

    def wardrive_log(self) -> list:
        """GPS-tagged AP sightings accumulated while a fix was valid. Real: it's
        the correlation of the capture stream with the GPS stream we receive."""
        return list(self._wardrive)

    # ---- capture-to-dump reassembly (Tier 2) --------------------------------
    # A blade doing a capture-to-dump job streams FH_OD_DUMP_IND chunks BEFORE
    # its summary RESP (the hub's flight completes on the RESP, so all chunks
    # are already mirrored here by the time the job is "done"). Chunks with
    # more=1 follow; the chunk with more=0 finalizes the stream.
    _DUMP_REC_CC1101 = 5     # u32 dur_us LE + u8 level
    _DUMP_REC_NRF24 = 40     # u32 t_ms LE + u8 len + payload[32] + 3 reserved

    def _on_dump_ind(self, ind: dict):
        seq = ind["seq"]
        buf = self._dumps_active.get(seq)
        if buf is None:
            buf = self._dumps_active[seq] = {
                "radio": ind["radio"], "chunks": [], "seen_idx": -1}
        if ind["chunk"] != buf["seen_idx"] + 1:
            if ind["chunk"] == buf["seen_idx"]:
                # Duplicate: a poll can clock the same staged chunk twice if a
                # poll beats loop()'s advance. Harmless — drop it.
                return
            # A real gap would mean a lost transaction — record the hole
            # honestly rather than silently concatenating two wrong streams.
            buf["gap"] = (buf["seen_idx"] + 1, ind["chunk"])
        buf["seen_idx"] = ind["chunk"]
        data = bytes(ind["data"])[: ind["len"]]
        buf["chunks"].append((ind["chunk"], data))
        if ind["more"]:
            return
        # finalize
        self._dumps_active.pop(seq, None)
        blob = b"".join(c for _, c in sorted(buf["chunks"]))
        dump = {"seq": seq, "radio": ind["radio"], "chunks": len(buf["chunks"]),
                "bytes": len(blob), "gap": buf.get("gap")}
        if ind["radio"] == self.p.defines["FH_DUMP_RADIO_CC1101"]:
            dump["records"] = [(int.from_bytes(blob[i:i+4], "little"), blob[i+4])
                               for i in range(0, len(blob) - 4, self._DUMP_REC_CC1101)]
            dump["kind"] = "ook-edges"
        elif ind["radio"] == self.p.defines["FH_DUMP_RADIO_NRF24"]:
            recs = []
            for i in range(0, len(blob) - 5, self._DUMP_REC_NRF24):
                t_ms = int.from_bytes(blob[i:i+4], "little")
                # len is wire-supplied; the record reserves payload[32], so a
                # corrupt chunk could claim >32 and over-read the next record.
                # Clamp to the frozen 32-byte field. (multi-check 2026-11-01)
                plen = min(blob[i+4], 32)
                recs.append((t_ms, blob[i+5:i+5+plen]))
            dump["records"] = recs
            dump["kind"] = "nrf24-payloads"
        else:
            dump["records"] = []
            dump["kind"] = f"raw(radio={ind['radio']})"
        if len(self._dumps) >= self._dumps_max:
            self._dumps.pop(next(iter(self._dumps)))
        self._dumps[seq] = dump

    def dumps(self) -> list:
        """Finalized capture-to-dump dumps, newest-first. Each: seq, radio,
        kind, chunk/record/byte counts, and 'gap' if a chunk was lost."""
        return sorted(self._dumps.values(), key=lambda d: d["seq"], reverse=True)

    def dump_records(self, seq: int) -> list:
        """The parsed records of one finalized dump (radio-layout-dependent)."""
        d = self._dumps.get(seq)
        return d["records"] if d else []

    def _dump_records_hex(self, seq: int, limit: int = 12) -> list:
        d = self._dumps.get(seq)
        if not d:
            return []
        if d["kind"] == "ook-edges":
            return [f"{dur:>7}us lv{l}" for dur, l in d["records"][:limit]]
        return [f"{t:>6}ms [{len(pl)}B] {pl.hex(':')}" for t, pl in d["records"][:limit]]

    # ---- the verbs (all BUSY-serialized) ------------------------------------
    def ping(self, timeout=2.0) -> Readiness:
        seq = self.s.next_seq()
        pl = self.p.s("fh_od_ping_req_t").pack(seq=seq)
        r = self._req("FH_OD_PING_REQ", pl, "FH_OD_PING_RESP", timeout)
        return Readiness(status=self._status(r["status"]),
                         radios_ready=self._radios(r["radios_ready"]))

    def gps(self):
        """Latest GPS fix. GPS also streams unsolicited (~1 Hz) via on_gps();
        this returns the most recent fix the client has seen, or None yet."""
        return getattr(self, "_last_gps", None)

    def sample_cc1101(self, freq_khz: int, dwell_ms: int = 2000, action: int = 1, timeout=None):
        seq = self.s.next_seq()
        pl = self.p.s("fh_od_cc1101_req_t").pack(seq=seq, action=action,
                                                 freq_khz=freq_khz, dwell_ms=dwell_ms)
        r = self._req("FH_OD_CC1101_REQ", pl, "FH_OD_CC1101_RESP",
                      (dwell_ms/1000 + 2) if timeout is None else timeout)
        self._remember_capture(seq, freq_khz, r)
        return r

    def _remember_capture(self, seq: int, freq_khz: int, r: dict):
        """Keep the decode triple (proto/bits/value) of a successful CC1101
        capture RESP so `cc1101 replay <freq> <seq>` can re-transmit it later.
        RESP data layout (blade): [0]=proto, [1]=bits, [2..5]=value u32 LE."""
        if r.get("status") != 0:            # FH_OD_OK only
            return
        d = bytes(r.get("data", b""))[: max(r.get("len", 0), 0)]
        if len(d) < 6:
            return
        if len(self._cc_captures) >= self._cc_captures_max:
            # Evict by INSERTION ORDER (true FIFO), not min(seq): seq is a
            # cycling 1..255 counter, so min() evicts the wrong entry after a
            # wrap. Matches _dumps/_obs eviction. (multi-check 2026-11-01)
            self._cc_captures.pop(next(iter(self._cc_captures)))
        self._cc_captures[seq] = {
            "seq": seq, "freq_khz": freq_khz, "proto": d[0], "bits": d[1],
            "value": int.from_bytes(d[2:6], "little"), "t": time.time(),
        }

    def cc1101_raw(self, freq_khz: int, dwell_ms: int = 5000, timeout=None):
        """RAW OOK capture-to-dump (action 2): the blade captures raw edge
        timings for the dwell window, streams them here as FH_OD_DUMP_IND
        chunks (resembled into self._dumps), then answers with a summary RESP.
        This is the UNBOUNDED listen (up to the blade's 30 s clamp)."""
        seq = self.s.next_seq()
        pl = self.p.s("fh_od_cc1101_req_t").pack(seq=seq, action=2,
                                                 freq_khz=freq_khz, dwell_ms=dwell_ms)
        r = self._req("FH_OD_CC1101_REQ", pl, "FH_OD_CC1101_RESP",
                      (dwell_ms/1000 + 2) if timeout is None else timeout)
        self._remember_capture(seq, freq_khz, r)
        return r

    def cc1101_replay(self, freq_khz: int, timeout=5.0):
        """REPLAY (action 3): re-transmit the LAST captured OOK signal on the
        CC1101 blade, at freq_khz. The blade holds the decode triple from the
        last successful capture (proto/bits/value); the RESP echoes it so the
        operator sees exactly what was re-emitted. ACTIVE verb — authorization
        lives in the operator (and the confirm gate in the intents layer)."""
        seq = self.s.next_seq()
        pl = self.p.s("fh_od_cc1101_req_t").pack(seq=seq, action=3,
                                                 freq_khz=freq_khz, dwell_ms=0)
        return self._req("FH_OD_CC1101_REQ", pl, "FH_OD_CC1101_RESP", timeout)

    def cc1101_replay_from(self, seq: int, repeats: int = 3, timeout=3.0):
        """Replay a SPECIFIC captured signal from Medusa's own decode-triple
        store (`cc1101_replay_from(seq)`; `cc1101_captures()` lists them).
        Fires the implemented 0x34 ext-band TX verb with the stored triple —
        so the blade does NOT need to still hold the capture (it may have
        re-captured or power-cycled since). The blade re-enters RX after.
        Raises KeyError if seq is unknown."""
        c = self._cc_captures.get(seq)
        if c is None:
            raise KeyError(f"no stored capture with seq {seq} "
                           f"(have: {sorted(self._cc_captures)})")
        r = self.cc1101_tx(c["freq_khz"], c["proto"], c["bits"], c["value"],
                           repeats, timeout)
        r["label"] = "CC1101 REPLAY-FROM"
        r["source_seq"] = seq
        return r

    def cc1101_captures(self) -> list:
        """The stored decode triples, newest-first. `cc1101 captures` verb."""
        return sorted(self._cc_captures.values(), key=lambda c: c["t"],
                      reverse=True)

    def sample_nrf24(self, chan_lo: int, chan_hi: int, dwell_ms: int = 200, timeout=None):
        seq = self.s.next_seq()
        pl = self.p.s("fh_od_nrf24_req_t").pack(seq=seq, mode=0, chan_lo=chan_lo,
                                                chan_hi=chan_hi, dwell_ms=dwell_ms)
        span = max(1, chan_hi - chan_lo + 1)
        return self._req("FH_OD_NRF24_REQ", pl, "FH_OD_NRF24_RESP",
                         (span*dwell_ms/1000 + 2) if timeout is None else timeout)

    def sniff_nrf24(self, chan: int, dwell_ms: int = 5000, timeout=None):
        """SNIFF capture-to-dump (mode 1): dwell on ONE channel for dwell_ms,
        capture every payload seen (nRF24L01+ payloads are 1..32 B), stream it
        here as FH_OD_DUMP_IND chunks, then answer with a summary RESP."""
        seq = self.s.next_seq()
        pl = self.p.s("fh_od_nrf24_req_t").pack(seq=seq, mode=1, chan_lo=chan,
                                                chan_hi=chan, dwell_ms=dwell_ms)
        return self._req("FH_OD_NRF24_REQ", pl, "FH_OD_NRF24_RESP",
                         (dwell_ms/1000 + 2) if timeout is None else timeout)

    # ---- extern verb band (Tier 3: transmit-disruption, authored externally) -
    # The blades answer with a generic RESP (status FH_OD_EXT_STUB until the
    # external author implements the verb). No menu entries — command-line only.
    _EXT_VERBS = {  # (radio, name) -> (REQ type, human label)
        ("cc1101", "jam"):        ("FH_OD_EXT_CC_JAM_REQ",   "CC1101 JAM"),
        ("cc1101", "brute"):      ("FH_OD_EXT_CC_BRUTE_REQ", "CC1101 BRUTE"),
        ("cc1101", "tx"):         ("FH_OD_EXT_CC_TX_REQ",    "CC1101 TX"),
        ("nrf24",  "jam"):        ("FH_OD_EXT_NRF_JAM_REQ",  "nRF24 JAM"),
        ("nrf24",  "protokill"):  ("FH_OD_EXT_NRF_PROTOKILL_REQ", "nRF24 PROTOKILL"),
        ("nrf24",  "tx"):         ("FH_OD_EXT_NRF_TX_REQ",   "nRF24 TX"),
    }

    def ext_verb(self, radio: str, verb: str, p1: int = 0, p2: int = 0,
                 p3: int = 0, tail: bytes = b"", timeout=None):
        """Fire an extern-band verb (Tier 3). p1/p2/p3 are the frozen generic
        params; p3 = duration_ms (0 = instant). The blade may answer EXT_STUB
        (plumbing works, implementation not authored) or a real status once the
        external author's firmware is flashed."""
        key = (radio, verb)
        if key not in self._EXT_VERBS:
            raise ValueError(f"unknown extern verb: {radio} {verb}")
        req_type, label = self._EXT_VERBS[key]
        req_id = self.p.type_by_name[req_type]        # numeric 0x30..0x3E (even)
        seq = self.s.next_seq()
        pl = self.p.s("fh_od_ext_req_t").pack(seq=seq, verb_id=req_id & 0xFF,
                                              p1=p1, p2=p2, p3=p3, tail=tail)
        # verb_id = the REQ's low byte; the blade matches its own band entries.
        r = self._req(req_type, pl, req_type.replace("_REQ", "_RESP"),
                      (p3/1000 + 2) if (timeout is None and p3) else (timeout or 3.0))
        r["label"] = label
        return r

    def cc1101_tx(self, freq_khz: int, proto: int, bits: int, value: int,
                  repeats: int = 3, timeout=3.0):
        """TX raw OOK (extern band 0x34, IMPLEMENTED): transmit author-specified
        bits at freq_khz — trigger your own relay/sensor, test your own
        receiver. Same transmitCode() core the REPLAY verb uses. The blade
        re-enters RX after the TX, so capture keeps working.
        tail[0]=proto, tail[1..4]=value u32 LE, tail[5]=repeats (0 -> 3)."""
        if not (1 <= bits <= 32):
            raise ValueError("bits must be 1..32")
        tail = bytes([proto & 0xFF]) + (value & 0xFFFFFFFF).to_bytes(4, "little") \
               + bytes([repeats & 0xFF])
        return self.ext_verb("cc1101", "tx", p1=freq_khz, p2=bits, p3=0,
                             tail=tail, timeout=timeout)

    def nrf24_tx(self, chan: int, payload: bytes, repeats: int = 1, timeout=3.0):
        """TX a payload (extern band 0x3C, IMPLEMENTED): transmit a 1..32-byte
        payload on a channel to your own receiver. Tail[0]=payload_len,
        tail[1..]=payload."""
        payload = bytes(payload)
        if not (1 <= len(payload) <= 32):
            raise ValueError("nRF24 payload must be 1..32 bytes")
        if not (0 <= chan <= 125):
            raise ValueError("channel must be 0..125")
        tail = bytes([len(payload)]) + payload
        return self.ext_verb("nrf24", "tx", p1=chan, p2=repeats & 0xFFFF, p3=0,
                             tail=tail, timeout=timeout)

    def read_nfc(self, timeout_ms: int = 2000, timeout=None):
        seq = self.s.next_seq()
        pl = self.p.s("fh_od_nfc_req_t").pack(seq=seq, timeout_ms=timeout_ms)
        return self._req("FH_OD_NFC_REQ", pl, "FH_OD_NFC_RESP",
                         (timeout_ms/1000 + 2) if timeout is None else timeout)

    def lora_tx(self, data: bytes, flags: int = 0, timeout=3.0):
        seq = self.s.next_seq()
        pl = self.p.s("fh_od_lora_tx_req_t").pack(seq=seq, flags=flags,
                                                  len=len(data), data=data)
        return self._req("FH_OD_LORA_TX_REQ", pl, "FH_OD_LORA_TX_RESP", timeout)

    def system_state(self, timeout=2.5):
        """Hub self-report: SD/uptime/config/job health. The HUB answers this
        REQ itself (0x2D/0x2E) — it is NOT routed to a blade. Everything in the
        RESP is data the hub already computes (logger stats, config snapshot,
        uptime, GPS fix cache); this verb just gives it a wire voice."""
        seq = self.s.next_seq()
        pl = self.p.s("fh_od_sys_state_req_t").pack(seq=seq)
        return self._req("FH_OD_SYS_STATE_REQ", pl, "FH_OD_SYS_STATE_RESP", timeout)


def resp_name_to_struct(resp_name: str) -> str:
    """'FH_OD_NRF24_RESP' -> 'fh_od_nrf24_resp_t'.
    The EXTERN band's RESPs all share the generic fh_od_ext_resp_t."""
    if resp_name.startswith("FH_OD_EXT_"):
        return "fh_od_ext_resp_t"
    return resp_name.lower() + "_t"   # FH_OD_NRF24_RESP -> fh_od_nrf24_resp_t
