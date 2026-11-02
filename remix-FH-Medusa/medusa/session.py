"""
medusa.session — serial transport + the frame fan-out + the BUSY lock.

Two-natured channel:
  * UP (unsolicited) — CAPTURE / GPS_FIX / SUMMARY / HEARTBEAT / LORA_RX_IND.
    These are delivered to registered event callbacks.
  * DOWN (request/response) — a *_REQ we send; the matching *_RESP (same seq)
    is delivered to the caller waiting on it.

One reader thread owns the serial port read side and routes every decoded frame
to exactly one place (an event callback, or the one request awaiting a reply).

BUSY contract: the Pico services ONE radio op at a time. This layer owns a single
command lock so callers physically cannot overlap on-demand radio requests — the
hardware's rule is enforced here, not left to the UI's good behavior.
"""
from __future__ import annotations
import threading
import time
import itertools
from dataclasses import dataclass
from .protocol import Protocol
from .codec import Codec, Frame

try:
    import serial          # pyserial
    _HAVE_SERIAL = True
except Exception:          # importable without the dep; connect() will explain
    _HAVE_SERIAL = False


class Busy(Exception):
    """Raised when an on-demand request is issued while another is in flight."""


class Timeout(Exception):
    """Raised when a request's matching RESP does not arrive in time."""


@dataclass
class Pending:
    seq: int
    resp_types: set[int]
    event: threading.Event
    frame: Frame | None = None


class Session:
    def __init__(self, proto: Protocol, port: str, baud: int = 921600):
        self.p = proto
        self.codec = Codec(proto)
        self.port = port
        self.baud = baud
        self._ser = None
        self._rx_thread = None
        self._stop = threading.Event()
        self._seq = itertools.cycle(range(1, 256))
        self._pending: Pending | None = None
        self._pending_lock = threading.Lock()
        self._cmd_lock = threading.Lock()      # the one-at-a-time BUSY gate
        self._write_lock = threading.Lock()    # serialize ALL writes to _ser (MULDER-8)
        self._event_cbs: list = []             # callables(Frame)
        self.link_lost = False                 # True once the reader gives up (port truly gone)
        # which TYPE codes are unsolicited events (everything not a _RESP)
        self._resp_types = {v for k, v in proto.type_by_name.items()
                            if k.endswith("_RESP")}

    # ---- lifecycle ----------------------------------------------------------
    def connect(self):
        if not _HAVE_SERIAL:
            raise RuntimeError("pyserial not installed — `pip install pyserial`. "
                               "See README; the codec/protocol layers work without it.")
        # BENCH FIX (2026-10-30): open with DTR/RTS released. On the P4 board
        # those lines drive the CH343 auto-reset circuit (EN / BOOT), and
        # pyserial's default (both asserted on open) rebooted the hub on every
        # Medusa launch -- losing ~10 s of BLE warm-up each time.
        ser = serial.Serial()
        ser.port, ser.baudrate, ser.timeout = self.port, self.baud, 0.1
        ser.dtr = False
        ser.rts = False
        ser.open()
        self._ser = ser
        self._stop.clear()
        self.link_lost = False
        self._rx_thread = threading.Thread(target=self._reader, daemon=True)
        self._rx_thread.start()

    def close(self):
        self._stop.set()
        if self._rx_thread:
            self._rx_thread.join(timeout=1)
        if self._ser:
            self._ser.close()

    def next_seq(self) -> int:
        return next(self._seq)

    def on_event(self, cb):
        """Register a callback(Frame) for unsolicited (event) frames."""
        self._event_cbs.append(cb)

    # ---- reader thread: route each frame to event or pending response -------
    def _reader(self):
        errors = 0
        while not self._stop.is_set():
            try:
                data = self._ser.read(256)
                errors = 0
            except Exception:
                # transient read hiccup: back off briefly and retry, do NOT kill
                # the reader on a single error (ERR-3). Give up only after a run
                # of consecutive failures (port truly gone).
                errors += 1
                if errors > 50:
                    # port is truly gone (unplugged) — surface it immediately so
                    # the UI doesn't sit on a stale "attached"/green link waiting
                    # on a freshness timeout a dead reader can never satisfy.
                    self.link_lost = True
                    break
                time.sleep(0.05)
                continue
            if not data:
                continue
            try:
                for frame in self.codec.feed(data):
                    self._route(frame)
            except Exception:
                pass   # a bad frame must never kill the reader

    def _route(self, frame: Frame):
        with self._pending_lock:
            pend = self._pending
            if pend and frame.type in pend.resp_types and self._seq_of(frame) == pend.seq:
                pend.frame = frame
                pend.event.set()
                self._pending = None
                return
        # otherwise it's an unsolicited event
        for cb in self._event_cbs:
            try:
                cb(frame)
            except Exception:
                pass

    def _seq_of(self, frame: Frame) -> int:
        # every REQ/RESP payload begins with a seq byte (fh_ondemand.h rule)
        return frame.payload[0] if frame.payload else -1

    # ---- request/response with BUSY enforcement -----------------------------
    def request(self, req_type: int, payload: bytes,
                resp_type: int, timeout: float = 2.0) -> Frame:
        """Send a *_REQ, block until the matching *_RESP (same seq) or timeout.
        Enforces one-at-a-time: raises Busy if another request holds the lock."""
        if not self._cmd_lock.acquire(blocking=False):
            raise Busy("another on-demand request is in flight (one-at-a-time)")
        try:
            seq = payload[0]
            pend = Pending(seq=seq, resp_types={resp_type}, event=threading.Event())
            with self._pending_lock:
                self._pending = pend
            with self._write_lock:
                self._ser.write(self.codec.encode(req_type, payload))
            got = pend.event.wait(timeout)
            with self._pending_lock:
                # Only clear the slot if it is STILL ours (a later request may
                # have replaced it; the reader clears it on a match). This closes
                # the timeout-vs-late-arrival race (ERR-1) and prevents a stale
                # RESP from ever satisfying a different request (ERR-2).
                if self._pending is pend:
                    self._pending = None
            if not got and pend.frame is None:
                raise Timeout(f"no RESP 0x{resp_type:02X} for seq {seq} in {timeout}s")
            if pend.frame is None:
                # event set but no frame (shouldn't happen) — treat as timeout
                raise Timeout(f"no RESP 0x{resp_type:02X} for seq {seq} (spurious wake)")
            return pend.frame
        finally:
            self._cmd_lock.release()

    def send_fire_and_forget(self, type_code: int, payload: bytes = b""):
        with self._write_lock:
            self._ser.write(self.codec.encode(type_code, payload))
