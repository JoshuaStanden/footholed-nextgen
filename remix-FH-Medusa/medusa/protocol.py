"""
medusa.protocol — the DRIFT-PROOF single source of truth.

This module does NOT hand-copy the wire layout. It PARSES the vendored C headers
(protocol/fh_frame.h + fh_ondemand.h — byte-identical copies of the firmware's
frozen headers) and derives every enum value, #define, and packed-struct layout
from them at import time.

WHY: the firmware (C) and this host app (Python) must agree on the exact bytes on
the wire. If a human re-typed the struct offsets into Python, the two copies would
"drift" the first time a field changed in C and nobody updated Python — Medusa
would then silently misread every frame. By parsing the C header itself, Python
cannot disagree with it: change the struct in C, re-vendor the header, and Medusa
re-derives the new layout (or fails loudly if it no longer matches the header's
own _Static_assert size guard).

SCOPE: this parser understands the SPECIFIC, disciplined style these two headers
use (packed structs of fixed-width <stdint.h> types, simple enums, simple
#defines). It is NOT a general C parser and is not meant to be. If the headers
ever adopt a construct it does not understand, it raises rather than guessing.
"""
from __future__ import annotations
import re
import struct
from pathlib import Path
from dataclasses import dataclass, field

PROTO_DIR = Path(__file__).resolve().parent.parent / "protocol"

# ---- C fixed-width type -> (python struct code, size bytes) ------------------
# little-endian is applied once, at the struct level ('<').
_CTYPE = {
    "uint8_t":  ("B", 1), "int8_t":  ("b", 1),
    "uint16_t": ("H", 2), "int16_t": ("h", 2),
    "uint32_t": ("I", 4), "int32_t": ("i", 4),
    "uint64_t": ("Q", 8), "int64_t": ("q", 8),
}


class DriftError(RuntimeError):
    """Raised when a parsed struct size disagrees with the header's own
    _Static_assert — i.e. the vendored header and the parser have drifted, or
    the header changed in a way this parser does not model. Loud by design."""


@dataclass
class CField:
    name: str
    ctype: str
    count: int          # array length (1 for scalar)


@dataclass
class CStruct:
    name: str
    fields: list[CField] = field(default_factory=list)

    @property
    def struct_format(self) -> str:
        """A little-endian struct.Struct format string for this packed struct."""
        fmt = "<"
        for f in self.fields:
            code, _ = _CTYPE[f.ctype]
            fmt += (str(f.count) + ("s" if f.ctype in ("uint8_t", "int8_t")
                                    and f.count > 1 else code)) if f.count > 1 else code
        return fmt

    @property
    def size(self) -> int:
        return struct.calcsize(self.struct_format)

    def unpack(self, payload: bytes) -> dict:
        """Decode a payload (exactly self.size bytes) into {field: value}.
        Byte arrays (uint8_t[N], e.g. mac, uid, data) come back as bytes."""
        # BENCH FIX (2026-10-30): the frozen contract lets a sender emit a
        # SHORT payload (e.g. CAPTURE LEN = FH_CAPTURE_FIXED + E, E < 10);
        # receivers must use LEN, not sizeof. struct.unpack demanded the full
        # size, raised, and _route swallowed it, so every real CAPTURE was
        # silently dropped. Zero-pad the unused tail (unsent extra = 0).
        payload = bytes(payload[: self.size]).ljust(self.size, b"\x00")
        vals = struct.unpack(self.struct_format, payload)
        out, i = {}, 0
        for f in self.fields:
            if f.count > 1 and f.ctype in ("uint8_t", "int8_t"):
                out[f.name] = vals[i]      # a bytes object
            elif f.count > 1:
                out[f.name] = vals[i:i + f.count]
                i += f.count - 1
            else:
                out[f.name] = vals[i]
            i += 1
        return out

    def pack(self, **kw) -> bytes:
        """Encode {field: value} into the packed little-endian payload."""
        args = []
        for f in self.fields:
            v = kw.get(f.name, 0)
            if f.count > 1 and f.ctype in ("uint8_t", "int8_t"):
                b = bytes(v) if not isinstance(v, (bytes, bytearray)) else bytes(v)
                args.append(b.ljust(f.count, b"\x00")[: f.count])
            else:
                args.append(v)
        return struct.pack(self.struct_format, *args)


# --------------------------------------------------------------------- parsing
_DEFINE_RE = re.compile(r"^\s*#define\s+(\w+)\s+(.+?)\s*(?:/\*.*)?$")
_ENUM_RE = re.compile(r"typedef\s+enum\s*\{(.*?)\}\s*(\w+)\s*;", re.S)
_STRUCT_RE = re.compile(
    r"typedef\s+struct\s+__attribute__\(\(packed\)\)\s*\{(.*?)\}\s*(\w+)\s*;", re.S)
_ASSERT_RE = re.compile(r"_Static_assert\s*\(\s*sizeof\s*\(\s*(\w+)\s*\)\s*==\s*(\d+)")
_ARRAY_EXPR = r"[\w\s\-\+]+"   # array size: name, int, or `NAME - N` arithmetic
_FIELD_RE = re.compile(rf"^\s*(\w+)\s+(\w+)\s*(?:\[\s*({_ARRAY_EXPR})\s*\])?\s*;")


def _strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    text = re.sub(r"//.*", "", text)
    return text


def _parse_defines(text: str) -> dict:
    out = {}
    for line in text.splitlines():
        m = _DEFINE_RE.match(line)
        if not m:
            continue
        name, val = m.group(1), m.group(2).strip()
        v = _eval_int_define(val)
        if v is not None:
            out[name] = v
    return out


def _eval_int_define(val: str):
    """Evaluate the SMALL set of integer #define forms these headers use:
    a plain int (hex/dec, optional u/U), optionally parenthesized, and a single
    left-shift `(1u << N)` used for the radio/bit bitmaps. Returns None otherwise.
    Deliberately NOT a general C expression evaluator — unknown forms are skipped."""
    s = val.strip().strip("()").strip()
    # plain integer, optional u/U suffix
    if re.fullmatch(r"0[xX][0-9a-fA-F]+[uU]?|[0-9]+[uU]?", s):
        return int(s.rstrip("uU"), 0)
    # single left shift: A << B  (e.g. 1u << 0)
    m = re.fullmatch(r"([0-9]+)[uU]?\s*<<\s*([0-9]+)[uU]?", s)
    if m:
        return int(m.group(1)) << int(m.group(2))
    return None


def _parse_enums(text: str) -> dict:
    enums = {}
    for body, tag in _ENUM_RE.findall(text):
        cur, members = -1, {}
        for item in body.split(","):
            item = item.strip()
            if not item:
                continue
            if "=" in item:
                nm, val = item.split("=", 1)
                cur = int(val.strip().rstrip("uU"), 0)
                members[nm.strip()] = cur
            else:
                cur += 1
                members[item] = cur
        enums[tag] = members
    return enums


def _array_count(expr: str, defines: dict):
    """Resolve a C array-size expression to an int count.

    Understands the SPECIFIC forms these headers use:
      * a plain integer literal            `10`
      * a single #define name              `FH_MAX_PAYLOAD`
      * name - literal / literal - name    `FH_MAX_PAYLOAD - 2`   (fh_lora_relay_t)
    Anything else returns None -> the caller raises DriftError (loud, never a
    silent wrong layout). This fixes the case where the old regex silently
    DROPPED `uint8_t data[FH_MAX_PAYLOAD - 2]`, making fh_lora_relay_t parse
    as 2 B instead of 255 B with no guard to catch it.
    """
    s = expr.strip()
    if re.fullmatch(r"\d+", s):
        return int(s, 0)
    if re.fullmatch(r"\w+", s):
        return defines.get(s)
    m = re.fullmatch(r"(\w+)\s*-\s*(\d+)", s)
    if m:
        base = defines.get(m.group(1))
        return base - int(m.group(2)) if base is not None else None
    m = re.fullmatch(r"(\d+)\s*-\s*(\w+)", s)
    if m:
        sub = defines.get(m.group(2))
        return int(m.group(1)) - sub if sub is not None else None
    return None


def _parse_structs(text: str, defines: dict) -> dict:
    structs = {}
    for body, tag in _STRUCT_RE.findall(text):
        st = CStruct(name=tag)
        for line in body.splitlines():
            m = _FIELD_RE.match(line.strip())
            if not m:
                continue
            ctype, name, arr = m.group(1), m.group(2), m.group(3)
            if ctype not in _CTYPE:
                raise DriftError(
                    f"{tag}.{name}: unmodelled C type '{ctype}' — the parser "
                    f"does not understand this; update medusa/protocol.py.")
            count = 1
            if arr is not None:
                count = _array_count(arr, defines)
                if count is None:
                    raise DriftError(f"{tag}.{name}: array size '{arr}' unresolved")
            st.fields.append(CField(name=name, ctype=ctype, count=count))
        structs[tag] = st
    return structs


def _check_drift(structs: dict, asserts: dict):
    """The guard: every struct the header asserts a size for MUST match what we
    parsed. A mismatch means the header and this parser have drifted — stop."""
    for name, expect in asserts.items():
        if name in structs and structs[name].size != expect:
            raise DriftError(
                f"DRIFT: {name} parsed as {structs[name].size} B but the header's "
                f"_Static_assert says {expect} B. The vendored header changed in a "
                f"way medusa/protocol.py does not model. Fix the parser; do NOT "
                f"hand-patch the layout.")


@dataclass
class Protocol:
    defines: dict
    enums: dict
    structs: dict
    type_by_name: dict      # 'FH_TYPE_GPS_FIX' -> 0x02
    type_name: dict         # 0x02 -> 'FH_TYPE_GPS_FIX'
    od_status: dict         # status code -> name

    def s(self, name: str) -> CStruct:
        return self.structs[name]


def load() -> Protocol:
    frame = _strip_comments((PROTO_DIR / "fh_frame.h").read_text())
    ondem = _strip_comments((PROTO_DIR / "fh_ondemand.h").read_text())
    raw_frame = (PROTO_DIR / "fh_frame.h").read_text()      # asserts need raw text
    raw_ondem = (PROTO_DIR / "fh_ondemand.h").read_text()

    defines = {**_parse_defines(frame), **_parse_defines(ondem)}
    enums = {**_parse_enums(frame), **_parse_enums(ondem)}
    structs = {**_parse_structs(frame, defines),
               **_parse_structs(ondem, defines)}

    asserts = {m.group(1): int(m.group(2))
               for m in _ASSERT_RE.finditer(raw_frame + raw_ondem)}
    _check_drift(structs, asserts)     # <-- the drift guard fires here

    fh_type = enums.get("fh_type_t", {})
    od_type = enums.get("fh_od_type_t", {})
    type_by_name = {**fh_type, **od_type}
    type_name = {v: k for k, v in type_by_name.items()}
    od_status = {v: k for k, v in enums.get("fh_od_status_t", {}).items()}

    return Protocol(defines=defines, enums=enums, structs=structs,
                    type_by_name=type_by_name, type_name=type_name,
                    od_status=od_status)
