"""
medusa.simulator — a FAKE HUB so you can run/develop Medusa with NO hardware.

It presents the same read(n)/write(bytes)/close() surface a pyserial port does,
so you drop it into Session in place of the real port. It:
  * emits a realistic fh_protocol stream — CAPTUREs from a small device
    population (so dedup shows "seen N×"), periodic GPS fixes, heartbeats, BLE;
  * answers on-demand REQs (ping/cc1101/nrf24/nfc/lora) with plausible RESPs,
    honoring the one-at-a-time BUSY timing (a dwell takes real wall-clock time).

Drift-proof: it builds every frame via the SAME parsed protocol + codec the rest
of Medusa uses, so the simulator can never disagree with the wire format.

This is a DEV TOOL. It proves the host side end-to-end without a device; it does
NOT prove the firmware. When you have hardware, point Session at the real port
instead of this.
"""
from __future__ import annotations
import time
import random
import threading
from .protocol import load, Protocol
from .codec import Codec


# a small, fixed population so CAPTURE dedup looks real ("seen 3x" climbs)
_DEVICES = [
    # (src_id, band_chan, mac, label-ish frame_type)
    (1, 0x5A, bytes.fromhex("a4113f22cc01"), 0x40),   # c5  5GHz
    (1, 0x53, bytes.fromhex("8c2240aa0b02"), 0x40),   # c5  5GHz
    (2, 0x21, bytes.fromhex("d0176688ee03"), 0x40),   # xs3 2.4GHz
    (2, 0x26, bytes.fromhex("f2aa11bb2204"), 0x40),   # xs3 2.4GHz
    (5, 0x20, bytes.fromhex("5c9a3e1177b0"), 0x50),   # BT  hub-local BLE (src 5)
    (5, 0x20, bytes.fromhex("e0acbb4409c1"), 0x50),   # BT  hub-local BLE (src 5)
]


class SimHub:
    def __init__(self, seed: int = 0):
        self.p: Protocol = load()
        self.c = Codec(self.p)
        self.rng = random.Random(seed)
        self._out = bytearray()
        self._lock = threading.Lock()
        self._busy_until = 0.0            # wall-clock when the current radio op frees
        self._seen = {}                   # mac -> count (so SUMMARY can report it)
        self._t0 = time.time()
        self._last_gps = 0.0
        self._last_hb = 0.0

    # ---- serial-like surface ------------------------------------------------
    def read(self, n: int) -> bytes:
        self._tick()                      # generate due unsolicited traffic
        with self._lock:
            chunk, self._out = bytes(self._out[:n]), self._out[n:]
        if not chunk:
            time.sleep(0.02)
        return chunk

    def write(self, data: bytes):
        # decode the hub<-client command and queue a RESP
        for fr in self.c.feed(data):
            self._handle_req(fr)

    def close(self):
        pass

    # ---- unsolicited stream -------------------------------------------------
    def _emit(self, type_name, payload):
        with self._lock:
            self._out += self.c.encode(self.p.type_by_name[type_name], payload)

    def _emit_dump_chunks(self, seq, radio, blob, max_chunk=250):
        """Stream `blob` as FH_OD_DUMP_IND chunks (250 B each), then... the
        CALLER stages the summary RESP last (chunks-before-RESP ordering)."""
        chunks = [blob[i:i + max_chunk] for i in range(0, len(blob), max_chunk)] or [b""]
        for idx, c in enumerate(chunks):
            self._emit("FH_OD_DUMP_IND", self.p.s("fh_od_dump_ind_t").pack(
                seq=seq, radio=radio, chunk=idx,
                more=1 if idx < len(chunks) - 1 else 0,
                len=len(c), data=c.ljust(250, b"\x00")[:250]))

    def _tick(self):
        now = time.time()
        # SSID mappings (TYPE 0x04), emitted ONCE near startup. Includes a
        # deliberate DUPLICATE: two distinct BSSIDs both advertise "HomeNet",
        # so dupe-ssid detection has something real to flag.
        if not getattr(self, "_ssid_sent", False):
            self._ssid_sent = True
            ssids = [
                (bytes.fromhex("a4113f22cc01"), "HomeNet"),      # BSSID 1
                (bytes.fromhex("8c2240aa0b02"), "HomeNet"),      # BSSID 2 — TWIN
                (bytes.fromhex("d0176688ee03"), "GuestWiFi"),
                (bytes.fromhex("f2aa11bb2204"), ""),             # hidden SSID
            ]
            for mac, name in ssids:
                nm = name.encode()[:32]
                pl = self.p.s("fh_ssid_t").pack(
                    mac=mac, ssid_len=len(nm), ssid=nm.ljust(32, b"\x00"))
                self._emit("FH_TYPE_SSID", pl)
        # a CAPTURE every ~0.3s from a random known device (dedup climbs)
        if self.rng.random() < 0.35:
            src, band_chan, mac, ftype = self.rng.choice(_DEVICES)
            self._seen[mac] = self._seen.get(mac, 0) + 1
            pl = self.p.s("fh_capture_t").pack(
                src_id=src, band_chan=band_chan, frame_type=ftype,
                rssi=self.rng.randint(-85, -40), mac=mac,
                gps_time=0, lat=0, lon=0,
                count=self._seen[mac], extra=b"")
            self._emit("FH_TYPE_CAPTURE", pl)
        # GPS ~1 Hz
        if now - self._last_gps > 1.0:
            self._last_gps = now
            pl = self.p.s("fh_gps_fix_t").pack(
                gps_time=int(now), lat=407421100, lon=-739813900,
                sats=self.rng.randint(7, 11), fix_qual=1, reserved=0)
            self._emit("FH_TYPE_GPS_FIX", pl)
        # heartbeats from each node ~ every 2s
        if now - self._last_hb > 2.0:
            self._last_hb = now
            for src in (1, 2, 3):
                pl = self.p.s("fh_heartbeat_t").pack(
                    src_id=src, uptime_s=int(now - self._t0),
                    capture_count=sum(self._seen.values()),
                    dropped_keys=0, task_alive=1, free_heap_kb=180)
                self._emit("FH_TYPE_HEARTBEAT", pl)

    # ---- command handling (RESP with BUSY timing) ---------------------------
    def _busy(self) -> bool:
        return time.time() < self._busy_until

    def _handle_req(self, fr):
        name = self.p.type_name.get(fr.type, "")
        seq = fr.payload[0] if fr.payload else 0
        OK = self.p.enums["fh_od_status_t"]["FH_OD_OK"]
        BUSY = self.p.enums["fh_od_status_t"]["FH_OD_BUSY"]
        STUB = self.p.enums["fh_od_status_t"]["FH_OD_EXT_STUB"]
        BAD = self.p.enums["fh_od_status_t"]["FH_OD_BAD_REQ"]

        # Extern verb band (Tier 3, 0x30..0x3E even = REQ). The disruption verbs
        # still answer FH_OD_EXT_STUB (their real implementation is authored
        # externally and replaces this stub); the TX verbs (0x34 CC / 0x3C NRF)
        # are IMPLEMENTED and emulate the blades' behavior so the CLI is
        # testable end-to-end on the bench without hardware.
        if name and name.startswith("FH_OD_EXT_") and name.endswith("_REQ"):
            req = self.p.s("fh_od_ext_req_t").unpack(fr.payload)
            if name == "FH_OD_EXT_CC_TX_REQ":
                # Echo the blade's behavior: proto/bits/value echo or BAD_REQ.
                proto, bits, reps = req["tail"][0], req["p2"] & 0xFF, req["tail"][5] or 3
                ok = bits >= 1 and bits <= 32 and 0 < proto < 100
                self._busy_until = time.time() + 0.05
                time.sleep(0.05)
                self._emit("FH_OD_EXT_CC_TX_RESP", self.p.s("fh_od_ext_resp_t").pack(
                    seq=req["seq"], status=OK if ok else BAD,
                    len=6 if ok else 0,
                    data=bytes([proto, bits,
                                req["tail"][1], req["tail"][2],
                                req["tail"][3], req["tail"][4]]) if ok else b""))
                return
            if name == "FH_OD_EXT_NRF_TX_REQ":
                plen = req["tail"][0] if req["tail"] else 0
                ch = req["p1"] & 0xFF
                ok = ch <= 125 and 1 <= plen <= 32
                self._busy_until = time.time() + 0.05
                time.sleep(0.05)
                self._emit("FH_OD_EXT_NRF_TX_RESP", self.p.s("fh_od_ext_resp_t").pack(
                    seq=req["seq"], status=OK if ok else BAD,
                    len=1 + plen + 1 if ok else 0,
                    data=bytes([plen]) + req["tail"][1:1 + plen] + bytes([1]) if ok else b""))
                return
            self._busy_until = time.time() + (req["p3"] / 1000.0 if req["p3"] else 0.05)
            time.sleep(req["p3"] / 1000.0 if req["p3"] else 0.05)
            self._emit(name.replace("_REQ", "_RESP"), self.p.s("fh_od_ext_resp_t").pack(
                seq=req["seq"], status=STUB, len=0))
            return

        if name == "FH_OD_PING_REQ":
            d = self.p.defines
            ready = (d["FH_OD_RADIO_CC1101"] | d["FH_OD_RADIO_NRF24"]
                     | d["FH_OD_RADIO_PN532"] | d["FH_OD_RADIO_SX1262"])
            self._emit("FH_OD_PING_RESP",
                       self.p.s("fh_od_ping_resp_t").pack(seq=seq, status=OK, radios_ready=ready))
            return

        if name == "FH_OD_SYS_STATE_REQ":
            # the fake hub answers ITSELF (parity with the real bridge's
            # SYS_STATE case): plausible logger/req/cfg stats, no fix.
            self._emit("FH_OD_SYS_STATE_RESP", self.p.s("fh_od_sys_state_resp_t").pack(
                seq=seq, status=OK,
                uptime_s=int(time.time() - self._t0), interactive=1,
                sd_mounted=1, sd_free_mb=8192, sd_used_mb=2048,
                rows_staged=12, rows_committed=3420, files_rolled=0,
                dropped_queue=0, dropped_sd_down=0, res_written=4,
                jobs_done=3, job_timeouts=0, job_busy=1,
                gps_rate=0, brightness_pct=50, ble_scan_mode=0,
                relog_mode=0, relog_window_s=60, band_mask=7, dwell_ms=110,
                cfg_modified=0, override_count=0,
                fix_valid=0, lat_e7=0, lon_e7=0, sats=0))
            return

        if self._busy():
            # mirror the one-at-a-time contract: refuse with BUSY
            self._busy_resp(name, seq, BUSY)
            return

        if name == "FH_OD_CC1101_REQ":
            req = self.p.s("fh_od_cc1101_req_t").unpack(fr.payload)
            if req["action"] == 3:              # REPLAY (no dwell — a TX job)
                self._busy_until = time.time() + 0.5
                time.sleep(0.5)
                data = bytes([self.rng.randint(1, 9), 24]) + \
                       self.rng.randbytes(4)     # proto, bits, value LE
                self._emit("FH_OD_CC1101_RESP", self.p.s("fh_od_cc1101_resp_t").pack(
                    seq=seq, status=OK, rssi=self.rng.randint(-80, -50),
                    len=len(data), data=data))
            else:
                self._busy_until = time.time() + req["dwell_ms"] / 1000.0
                time.sleep(req["dwell_ms"] / 1000.0)   # respond after the listen
                if req["action"] == 2:
                    # RAW_OOK capture-to-dump: chunks FIRST, summary RESP last
                    # (the hub clocks one staged frame per poll; the flight only
                    # completes on the RESP, so chunks must come first).
                    recs = [(self.rng.randint(120, 4200), self.rng.randint(0, 1))
                            for _ in range(120)]
                    self._emit_dump_chunks(seq, self.p.defines["FH_DUMP_RADIO_CC1101"],
                                           b"".join(v.to_bytes(4, "little") + bytes([lv])
                                                    for v, lv in recs))
                    # Summary RESP: the blade keeps the decode triple + RSSI so
                    # a subsequent REPLAY works from a raw capture too.
                    data = bytes([self.rng.randint(1, 9), 24]) + \
                           self.rng.randbytes(4)   # proto, bits, value LE
                    self._emit("FH_OD_CC1101_RESP", self.p.s("fh_od_cc1101_resp_t").pack(
                        seq=seq, status=OK, rssi=self.rng.randint(-80, -50),
                        len=len(data), data=data))
                else:
                    # Blade capture RESP: len=6, data = proto, bits, value LE.
                    data = bytes([self.rng.randint(1, 9), 24]) + \
                           self.rng.randbytes(4)   # proto, bits, value LE
                    self._emit("FH_OD_CC1101_RESP", self.p.s("fh_od_cc1101_resp_t").pack(
                        seq=seq, status=OK, rssi=self.rng.randint(-80, -50),
                        len=len(data), data=data))
        elif name == "FH_OD_NRF24_REQ":
            req = self.p.s("fh_od_nrf24_req_t").unpack(fr.payload)
            if req["mode"] == 1:
                # SNIFF capture-to-dump: 40-byte records (u32 t_ms LE + u8 len +
                # payload[32] + 3 reserved), chunks first, RESP last.
                recs = []
                for i in range(18):
                    pl = bytes(self.rng.randint(0, 255) for _ in range(self.rng.randint(4, 32)))
                    recs.append(i.to_bytes(4, "little") + bytes([len(pl)])
                                + pl.ljust(32, b"\x00") + b"\x00\x00\x00")
                self._emit_dump_chunks(seq, self.p.defines["FH_DUMP_RADIO_NRF24"],
                                       b"".join(recs))
                self._emit("FH_OD_NRF24_RESP", self.p.s("fh_od_nrf24_resp_t").pack(
                    seq=seq, status=OK, active_chan=req["chan_lo"], rssi=0))
            else:
                span = max(1, req["chan_hi"] - req["chan_lo"] + 1)
                self._busy_until = time.time() + span * req["dwell_ms"] / 1000.0
                time.sleep(span * req["dwell_ms"] / 1000.0)   # respond after the scan
                self._emit("FH_OD_NRF24_RESP", self.p.s("fh_od_nrf24_resp_t").pack(
                    seq=seq, status=OK,
                    active_chan=self.rng.randint(req["chan_lo"], req["chan_hi"]),
                    rssi=self.rng.randint(-80, -50)))
        elif name == "FH_OD_NFC_REQ":
            # pretend a MIFARE Classic 1K is present (sak 0x08, 4-byte UID)
            uid = bytes.fromhex("04a1b2c3") + b"\x00" * 6
            self._emit("FH_OD_NFC_RESP", self.p.s("fh_od_nfc_resp_t").pack(
                seq=seq, status=OK, uid_len=4, uid=uid, tag_type=0x08))
        elif name == "FH_OD_LORA_TX_REQ":
            self._emit("FH_OD_LORA_TX_RESP", self.p.s("fh_od_lora_tx_resp_t").pack(
                seq=seq, status=OK, toa_ms=self.rng.randint(20, 120)))

    def _busy_resp(self, name, seq, BUSY):
        resp = name.replace("_REQ", "_RESP")
        if resp not in self.p.type_by_name:
            return
        struct_name = resp.lower() + "_t"
        # minimal BUSY resp: seq + status where the struct supports it
        st = self.p.s(struct_name)
        kw = {"seq": seq}
        if any(f.name == "status" for f in st.fields):
            kw["status"] = BUSY
        self._emit(resp, st.pack(**kw))
