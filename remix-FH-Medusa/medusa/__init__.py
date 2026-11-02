"""
FH_Medusa — experimental host-side bench face for the FH-Advanced system.

The hub forwards every node's fh_protocol frames over USB-serial; Medusa decodes
them live (event stream) and sends on-demand radio commands back (request/
response, BUSY-serialized). The wire layout is derived from the vendored frozen
headers at import time — see medusa/protocol.py — so this can never drift from
the firmware.

Public surface:
    from medusa import MedusaClient
    from medusa.session import Busy, Timeout
"""
from .client import MedusaClient, Readiness
from .protocol import load as load_protocol, Protocol, DriftError
from .codec import Codec, Frame
from .session import Session, Busy, Timeout

__all__ = ["MedusaClient", "Readiness", "load_protocol", "Protocol",
           "DriftError", "Codec", "Frame", "Session", "Busy", "Timeout"]
__version__ = "0.0.1-experimental"
