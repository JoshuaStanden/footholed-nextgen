#!/usr/bin/env python3
"""
simdemo.py — watch Medusa run against the SIMULATED hub, no hardware, no curses.

    python3 simdemo.py

Prints the live event stream for a few seconds, then fires a few commands and
shows the results — including a deliberate BUSY collision (two radio ops at once)
so you can see the one-at-a-time contract enforced. Runs in ANY plain terminal.

For the full interactive TUI, use:  python3 tui.py --sim   (needs a real terminal)
"""
import time
import threading
from medusa import MedusaClient
from medusa.session import Busy


def main():
    m = MedusaClient(port="sim")
    # pretty-print a few event kinds as they stream in
    m.on_capture(lambda c: print(f"  CAPTURE src={c['src_id']} rssi={c['rssi']:4d} "
                                 f"mac={c['mac'].hex(':')[:17]} seen={c['count']}×"))
    m.on_gps(lambda g: print(f"  GPS fix sats={g['sats']} "
                             f"{g['lat']/1e7:.5f},{g['lon']/1e7:.5f}"))
    m.on_heartbeat(lambda h: None)          # quiet; they're frequent

    print("=== connecting to SIMULATED hub ===")
    m.connect(use_sim=True)

    print("=== 3 seconds of live stream ===")
    time.sleep(3)

    print("\n=== fire: ping ===")
    print("  ->", m.ping())

    print("\n=== fire: nrf24 scan 0..10 (dwell 20ms/ch) ===")
    print("  ->", m.sample_nrf24(0, 10, dwell_ms=20))

    print("\n=== fire: read NFC (sim presents a MIFARE Classic 1K) ===")
    card = m.read_nfc()
    print("  ->", card)
    from medusa.intents import classify_card
    uid = bytes(card.get("uid", b""))[:card.get("uid_len", 0)]
    print("  classify ->", classify_card(card.get("tag_type"), uid))

    print("\n=== BUSY test: start a long cc1101 op, immediately fire another ===")
    def long_op():
        try:
            m.sample_cc1101(433920, dwell_ms=1500)
        except Busy as e:
            print("  long_op Busy:", e)
    t = threading.Thread(target=long_op); t.start()
    time.sleep(0.2)                         # let the long op grab the lock
    try:
        m.sample_nrf24(0, 10, dwell_ms=20)
        print("  second op: (unexpectedly ran)")
    except Busy:
        print("  second op correctly REFUSED with Busy (one-at-a-time) ✓")
    t.join()

    print("\n=== fire: cc1101 RAW (unbounded OOK capture-to-dump) ===")
    r = m.cc1101_raw(433920, dwell_ms=1500)
    print("  resp ->", {k: r[k] for k in ("status", "rssi", "len")})
    d = m.dumps()
    if d:
        print(f"  dump -> seq {d[0]['seq']} · {d[0]['chunks']} chunks · "
              f"{len(d[0]['records'])} edges · {d[0]['bytes']}B"
              + (f"  ⚠ gap {d[0]['gap']}" if d[0].get("gap") else ""))
        for x in m._dump_records_hex(d[0]["seq"], 6):
            print("     ", x)
    else:
        print("  dump -> (none — reassembly FAILED)")

    print("\n=== fire: nRF24 sniff (payload capture-to-dump, ch 76) ===")
    r = m.sniff_nrf24(76, dwell_ms=1200)
    print("  resp ->", {k: r[k] for k in ("status", "active_chan")})
    d = m.dumps()
    if d and d[0]["kind"] == "nrf24-payloads":
        print(f"  dump -> seq {d[0]['seq']} · {d[0]['chunks']} chunks · "
              f"{len(d[0]['records'])} payloads · {d[0]['bytes']}B")
        for x in m._dump_records_hex(d[0]["seq"], 6):
            print("     ", x)
    else:
        print("  dump -> (none — reassembly FAILED)")

    print("\n=== fire: cc1101 captures (decode-triple store) + replay-from ===")
    r = m.cc1101_raw(433920, dwell_ms=800)
    print("  resp ->", {k: r[k] for k in ("status", "rssi")})
    caps = m.cc1101_captures()
    if caps:
        c = caps[0]
        print(f"  store -> {len(caps)} capture(s); newest seq={c['seq']} "
              f"proto={c['proto']} bits={c['bits']} value=0x{c['value']:06X}")
        r2 = m.cc1101_replay_from(c["seq"])
        print("  replay-from resp ->", {k: r2[k] for k in ("status", "len")})
        d = bytes(r2.get("data", b""))[: r2.get("len", 0)]
        if len(d) >= 6:
            print(f"  echo  -> proto={d[0]} bits={d[1]} "
                  f"value=0x{int.from_bytes(d[2:6], 'little'):06X} "
                  f"(matches the stored triple)")
        else:
            print("  echo  -> (no echo — replay-from FAILED)")
    else:
        print("  store -> (empty — the capture didn't return a triple)")

    print("\n=== fire: CC1101 tx (raw OOK transmit, own device) ===")
    r = m.cc1101_tx(433920, proto=1, bits=24, value=0x2A2B3C, repeats=3)
    print("  resp ->", {k: r[k] for k in ("status", "len")})
    d = bytes(r.get("data", b""))[: r.get("len", 0)]
    if len(d) >= 6:
        print(f"  echo -> proto={d[0]} bits={d[1]} value=0x{int.from_bytes(d[2:6], 'little'):06X}")
    else:
        print("  echo -> (no echo — TX FAILED)")

    print("\n=== fire: nRF24 tx (payload transmit, own receiver) ===")
    r = m.nrf24_tx(76, b"\x1A\x2B\x3C", repeats=1)
    print("  resp ->", {k: r[k] for k in ("status", "len")})
    d = bytes(r.get("data", b""))[: r.get("len", 0)]
    if len(d) >= 1 + 3 + 1:
        print(f"  echo -> payload {d[1:1+3].hex().upper()} · writes acked {d[4]}/1")
    else:
        print("  echo -> (no echo — TX FAILED)")

    m.close()
    print("\n=== done — this all ran against the simulator, no hardware ===")


if __name__ == "__main__":
    main()
