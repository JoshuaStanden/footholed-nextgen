"""
medusa.codec — framing + CRC, mirroring fh_frame.h exactly.

Frame: SYNC(0xA5 0x5A) | TYPE(1) | LEN(1) | PAYLOAD(LEN) | CRC16(2)
CRC-16/CCITT-FALSE over TYPE..end-of-PAYLOAD (NOT the SYNC). Little-endian.

The decoder is resync-safe like the C one: a dropped byte costs at most one
frame, never stream alignment (length-driven + SYNC re-anchor).
"""
from __future__ import annotations
from dataclasses import dataclass
from .protocol import Protocol


def crc16_ccitt_false(data: bytes, poly: int, init: int) -> int:
    crc = init
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ poly) & 0xFFFF if (crc & 0x8000) else (crc << 1) & 0xFFFF
    return crc & 0xFFFF


@dataclass
class Frame:
    type: int
    payload: bytes


class Codec:
    def __init__(self, proto: Protocol):
        self.p = proto
        self.SYNC0 = proto.defines["FH_SYNC0"]
        self.SYNC1 = proto.defines["FH_SYNC1"]
        self.POLY = proto.defines["FH_CRC16_POLY"]
        self.INIT = proto.defines["FH_CRC16_INIT"]
        self.MAX_PAYLOAD = proto.defines["FH_MAX_PAYLOAD"]
        # decoder state
        self._buf = bytearray()
        self.good_frames = 0
        self.crc_errors = 0
        self.resyncs = 0

    def crc(self, data: bytes) -> int:
        return crc16_ccitt_false(data, self.POLY, self.INIT)

    # ---- encode one frame (bytes ready to write to the serial port) ----------
    def encode(self, type_code: int, payload: bytes = b"") -> bytes:
        if len(payload) > self.MAX_PAYLOAD:
            raise ValueError(f"payload {len(payload)} > FH_MAX_PAYLOAD {self.MAX_PAYLOAD}")
        body = bytes([type_code & 0xFF, len(payload)]) + payload
        c = self.crc(body)
        return bytes([self.SYNC0, self.SYNC1]) + body + bytes([c & 0xFF, (c >> 8) & 0xFF])

    # ---- streaming decode (feed bytes, yield complete validated frames) ------
    def feed(self, chunk: bytes):
        """Push received bytes; yield Frame for each complete CRC-valid frame.
        Resync-safe: scans for SYNC, length-driven, re-anchors on bad CRC."""
        self._buf.extend(chunk)
        buf = self._buf
        while True:
            # find SYNC0 SYNC1
            i = 0
            while i + 1 < len(buf) and not (buf[i] == self.SYNC0 and buf[i + 1] == self.SYNC1):
                i += 1
            if i:                       # discarded i bytes looking for sync
                self.resyncs += 1
                del buf[:i]
            if len(buf) < 4:            # need SYNC+TYPE+LEN
                return
            type_code = buf[2]
            length = buf[3]
            total = 4 + length + 2      # + CRC
            if len(buf) < total:
                return                  # wait for more bytes
            body = bytes(buf[2:4 + length])         # TYPE..end-of-PAYLOAD
            got_crc = buf[4 + length] | (buf[5 + length] << 8)
            if self.crc(body) == got_crc:
                self.good_frames += 1
                frame = Frame(type=type_code, payload=bytes(buf[4:4 + length]))
                del buf[:total]
                yield frame
            else:
                self.crc_errors += 1
                # bad CRC: drop the SYNC0 and re-scan (re-anchor)
                del buf[:1]
                self.resyncs += 1
