#!/usr/bin/env python3
"""
selftest.py — prove the Medusa stack WITHOUT hardware or pyserial.

Run:  python3 selftest.py
It exercises: the drift guard, CRC known-answer, event decoding, the BUSY lock,
and a simulated request/response round-trip over a fake loopback serial.

This is the "proven by running, not just reading" gate for the host side. The
firmware side is still unverified until you build it — Medusa only proves ITS
half (the protocol/codec/session/client logic) is correct.
"""
import threading
import time
from medusa import protocol, codec
from medusa.protocol import DriftError


def check(name, cond):
    print(f"  {'ok ' if cond else 'FAIL'}: {name}")
    if not cond:
        raise SystemExit(f"FAILED: {name}")


def main():
    print("FH_Medusa self-test (no hardware)\n---------------------------------")
    # 1. drift guard — load() raises if any struct != its _Static_assert
    p = protocol.load()
    check("protocol loads + drift guard passes", True)
    check("fh_gps_fix_t is 16 B", p.s("fh_gps_fix_t").size == 16)
    check("fh_od_lora_tx_req_t is 203 B", p.s("fh_od_lora_tx_req_t").size == 203)
    check("FH_TYPE_GPS_FIX == 0x02", p.type_by_name["FH_TYPE_GPS_FIX"] == 0x02)
    check("FH_OD_NRF24_REQ == 0x24", p.type_by_name["FH_OD_NRF24_REQ"] == 0x24)
    check("fh_od_nrf24_req_t is 6 B (mode added)", p.s("fh_od_nrf24_req_t").size == 6)
    check("FH_OD_DUMP_IND == 0x2F", p.type_by_name["FH_OD_DUMP_IND"] == 0x2F)
    check("fh_od_dump_ind_t is 255 B", p.s("fh_od_dump_ind_t").size == 255)

    # 2. CRC known-answer
    c = codec.Codec(p)
    check("CRC16/CCITT-FALSE(\"123456789\") == 0x29B1", c.crc(b"123456789") == 0x29B1)

    # 3. event decode: encode a heartbeat, feed it, confirm unpack
    hb = p.s("fh_heartbeat_t").pack(src_id=3, uptime_s=42, capture_count=7,
                                    dropped_keys=0, task_alive=1, free_heap_kb=180)
    frames = list(c.feed(c.encode(p.type_by_name["FH_TYPE_HEARTBEAT"], hb)))
    d = p.s("fh_heartbeat_t").unpack(frames[0].payload)
    check("heartbeat decodes (src_id=3, caps=7)", d["src_id"] == 3 and d["capture_count"] == 7)

    # 4. capture with byte array (mac) round-trips
    cap = p.s("fh_capture_t").pack(src_id=1, band_chan=0x5a, frame_type=0, rssi=-55,
                                   mac=bytes.fromhex("001122334455"), gps_time=0,
                                   lat=0, lon=0, count=1, extra=b"")
    f = list(c.feed(c.encode(p.type_by_name["FH_TYPE_CAPTURE"], cap)))[0]
    dd = p.s("fh_capture_t").unpack(f.payload)
    check("capture mac round-trips", dd["mac"].hex() == "001122334455" and dd["rssi"] == -55)

    # 4b. DUMP_IND chunk reassembly (the Tier-2 capture-to-dump store)
    from medusa.client import MedusaClient
    cl = MedusaClient.__new__(MedusaClient)      # no serial — only the reassembler
    cl.p = p; cl._dumps = {}; cl._dumps_active = {}; cl._dumps_max = 32
    di = p.s("fh_od_dump_ind_t")
    recs = b"".join(v.to_bytes(4, "little") + bytes([1]) for v in (100, 200, 300))
    for idx, (more, payload) in enumerate(((1, recs[:8]), (0, recs[8:]))):
        cl._on_dump_ind(di.unpack(di.pack(seq=5, radio=p.defines["FH_DUMP_RADIO_CC1101"],
                                          chunk=idx, more=more, len=len(payload), data=payload)))
    d = cl._dumps[5]
    check("DUMP_IND reassembles (2 chunks -> 3 OOK edges)",
          d["kind"] == "ook-edges" and d["records"] == [(100, 1), (200, 1), (300, 1)]
          and d["chunks"] == 2 and not d.get("gap"))

    # 5. BUSY lock + simulated REQ/RESP over a fake loopback serial
    from medusa.session import Session, Busy
    fake = FakeLoop(p)                      # echoes a RESP for each REQ
    sess = Session(p, port="fake"); sess._ser = fake; sess._stop.clear()
    t = threading.Thread(target=sess._reader, daemon=True); t.start()

    seq = sess.next_seq()
    pl = p.s("fh_od_nrf24_req_t").pack(seq=seq, mode=0, chan_lo=0, chan_hi=80, dwell_ms=50)
    rf = sess.request(p.type_by_name["FH_OD_NRF24_REQ"], pl,
                      p.type_by_name["FH_OD_NRF24_RESP"], timeout=2.0)
    resp = p.s("fh_od_nrf24_resp_t").unpack(rf.payload)
    check("REQ/RESP round-trips (seq echoed)", resp["seq"] == seq)
    check("RESP active_chan came back", resp["active_chan"] == 7)

    # BUSY: hold the command lock, a second request must raise Busy
    sess._cmd_lock.acquire()
    try:
        raised = False
        try:
            sess.request(p.type_by_name["FH_OD_PING_REQ"],
                         p.s("fh_od_ping_req_t").pack(seq=99),
                         p.type_by_name["FH_OD_PING_RESP"], timeout=0.3)
        except Busy:
            raised = True
        check("second concurrent request raises Busy (one-at-a-time)", raised)
    finally:
        sess._cmd_lock.release()
    sess._stop.set()
    t.join(timeout=1)          # ensure the old reader can't steal our RESPs

    # 6. CC1101 replay-from (decode-triple store): a capture stores its triple,
    # then the replay-from verb fires the implemented 0x34 ext-band TX verb
    # with the STORED triple — proving the blade does not need to still hold
    # the capture. FakeLoop answers both the capture RESP and the TX echo.
    # (Runs AFTER the fake session above is stopped — two readers on one
    # FakeLoop would steal each other's RESP bytes.)
    cl = MedusaClient.__new__(MedusaClient)
    cl.p = p
    cl.s = Session(p, port="fake"); cl.s._ser = fake; cl.s._stop.clear()
    cl._cc_captures = {}; cl._cc_captures_max = 32
    cl._results = []; cl._results_max = 64
    threading.Thread(target=cl.s._reader, daemon=True).start()
    r = cl.sample_cc1101(433920, dwell_ms=50, action=1)
    seq0 = next(iter(cl._cc_captures))
    cap = cl._cc_captures[seq0]
    check("capture triple stored (proto/bits/value)",
          cap["proto"] == 1 and cap["bits"] == 24
          and cap["value"] == 0x2A2B3C and cap["freq_khz"] == 433920)
    r = cl.cc1101_replay_from(seq0)
    d = bytes(r.get("data", b""))[: r.get("len", 0)]
    check("replay-from fires the 0x34 TX verb with the stored triple",
          r.get("status") == 0 and len(d) == 6
          and d[0] == 1 and d[1] == 24
          and int.from_bytes(d[2:6], "little") == 0x2A2B3C)
    raised = False
    try:
        cl.cc1101_replay_from(999)
    except KeyError:
        raised = True
    check("replay-from raises KeyError on an unknown seq", raised)
    cl.s._stop.set()

    print("---------------------------------\nALL SELF-TESTS PASSED")


class FakeLoop:
    """A stand-in serial object: when a *_REQ frame is written, it queues the
    matching *_RESP so the reader thread delivers it — simulates the Pico."""
    def __init__(self, p):
        self.p = p
        self.c = codec.Codec(p)
        self._out = bytearray()
        self._dec = codec.Codec(p)

    def write(self, data: bytes):
        for fr in self._dec.feed(data):
            name = self.p.type_name.get(fr.type, "")
            if name == "FH_OD_NRF24_REQ":
                seq = fr.payload[0]
                resp = self.p.s("fh_od_nrf24_resp_t").pack(
                    seq=seq, status=0, active_chan=7, rssi=-60)
                self._out += self.c.encode(self.p.type_by_name["FH_OD_NRF24_RESP"], resp)
            elif name == "FH_OD_CC1101_REQ":
                # Capture RESP with a FIXED decode triple (blade format:
                # data[0]=proto, [1]=bits, [2..5]=value u32 LE, len=6).
                seq = fr.payload[0]
                data = bytes([1, 24]) + (0x2A2B3C).to_bytes(4, "little")
                resp = self.p.s("fh_od_cc1101_resp_t").pack(
                    seq=seq, status=0, rssi=-55, len=len(data), data=data)
                self._out += self.c.encode(self.p.type_by_name["FH_OD_CC1101_RESP"], resp)
            elif name == "FH_OD_EXT_CC_TX_REQ":
                # The implemented 0x34 TX verb: echo proto/bits/value (6 bytes)
                # exactly like the blade does. Layout: seq, verb_id, p1 u32,
                # p2 u32, p3 u32, tail[0]=proto, tail[1..4]=value u32 LE.
                seq = fr.payload[0]
                bits = fr.payload[6]          # p2 low byte = bits
                proto = fr.payload[14]        # tail[0]
                value = int.from_bytes(fr.payload[15:19], "little")  # tail[1..4]
                data = bytes([proto, bits]) + value.to_bytes(4, "little")
                resp = self.p.s("fh_od_ext_resp_t").pack(
                    seq=seq, status=0, len=len(data), data=data)
                self._out += self.c.encode(
                    self.p.type_by_name["FH_OD_EXT_CC_TX_RESP"], resp)

    def read(self, n):
        time.sleep(0.01)
        chunk, self._out = bytes(self._out[:n]), self._out[n:]
        return chunk

    def close(self): pass


if __name__ == "__main__":
    main()
