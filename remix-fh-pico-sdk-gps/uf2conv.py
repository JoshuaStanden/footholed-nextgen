#!/usr/bin/env python3
"""
uf2conv.py — minimal UF2 converter for RP2040 (no dependencies, stdlib only).

Why this exists: PICO_NO_PICOTOOL=1 is set in CMakeLists.txt because this
machine has no host C/C++ compiler to build picotool. The Pico SDK still
produces the raw flash .bin (objcopy -Obinary); this script wraps it into a
bootable .uf2 that the RP2040 BOOTSEL mass-storage flasher accepts.

UF2 format (512-byte blocks, RP2040 family):
  Offset  Size  Field
  0x00      4   magic0 = 0x0A324655 ("UF2\n")
  0x04      4   magic1 = 0x9E5D5157
  0x08      4   flags: bit 12 (0x2000) = family ID present
  0x0C      4   flash address of this block's payload (base 0x10000000)
  0x10      4   payload size (256)
  0x14      4   block number (0-based)
  0x18      4   total number of blocks
  0x1C      4   file size in bytes (when family flag is set, RP2040 uses this
                word as the FAMILY ID instead; RP2040 family = 0xe48bff56)
  0x20    256   payload (zero-padded)
  0x120   220   zero padding (data region totals 476 bytes)
  0x1FC     4   magic2 = 0x0AB16F45
Total 512 bytes per block. A block is only valid if all three magics match.

Usage: python uf2conv.py build/fh_pico_gps.bin -o build/fh_pico_gps.uf2
"""
import struct, sys, os

MAGIC0 = 0x0A324655
MAGIC1 = 0x9E5D5157
MAGIC2 = 0x0AB16F45
FLAGS_FAMILY_PRESENT = 0x00002000
FAMILY_RP2040 = 0xe48bff56      # "family ID for RP2040" (USB PID 0x0003)
FLASH_BASE = 0x10000000         # RP2040 XIP flash window
BLOCK_SIZE = 512                # total UF2 block size
PAYLOAD_SIZE = 256              # data bytes per block

def convert_bin_to_uf2(bin_path, uf2_path):
    with open(bin_path, "rb") as f:
        data = f.read()
    # RP2040 flash images must be padded to a 256-byte boundary (per the SDK's
    # uf2 generation); round up whole 256-byte payload blocks.
    n_blocks = (len(data) + PAYLOAD_SIZE - 1) // PAYLOAD_SIZE
    blocks = []
    for i in range(n_blocks):
        payload = data[i * PAYLOAD_SIZE:(i + 1) * PAYLOAD_SIZE]
        payload += b"\x00" * (PAYLOAD_SIZE - len(payload))
        header = struct.pack(
            "<IIIIIIII",
            MAGIC0,                         # magic0
            MAGIC1,                         # magic1
            FLAGS_FAMILY_PRESENT,           # flags: family ID present
            FLASH_BASE + i * PAYLOAD_SIZE,  # flash address of this payload
            PAYLOAD_SIZE,                   # bytes of payload used
            i,                              # block number
            n_blocks,                       # total blocks
            FAMILY_RP2040,                  # family ID (in the filesize slot)
        )
        # data region = 476 bytes total; payload in first 256, rest zero.
        blocks.append(header + payload +
                      b"\x00" * (BLOCK_SIZE - len(header) - PAYLOAD_SIZE - 4) +
                      struct.pack("<I", MAGIC2))
    with open(uf2_path, "wb") as f:
        f.write(b"".join(blocks))
    return n_blocks, len(data)

def verify_uf2(uf2_path, expected_n_blocks):
    """Re-read the .uf2 and check every block's three magics + field sanity."""
    with open(uf2_path, "rb") as f:
        blob = f.read()
    assert len(blob) == expected_n_blocks * BLOCK_SIZE, "size mismatch"
    for i in range(expected_n_blocks):
        b = blob[i * BLOCK_SIZE:(i + 1) * BLOCK_SIZE]
        m0, m1, flags, addr, plen, blk, nblocks, fam = struct.unpack_from(
            "<IIIIIIII", b, 0)
        m2 = struct.unpack_from("<I", b, BLOCK_SIZE - 4)[0]
        assert m0 == MAGIC0 and m1 == MAGIC1 and m2 == MAGIC2, f"magic {i}"
        assert flags == FLAGS_FAMILY_PRESENT and fam == FAMILY_RP2040, f"fam {i}"
        assert addr == FLASH_BASE + i * PAYLOAD_SIZE, f"addr {i}"
        assert blk == i and nblocks == expected_n_blocks and plen == PAYLOAD_SIZE
    return True

if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("usage: uf2conv.py <input.bin> [-o output.uf2]")
    inp = sys.argv[1]
    outp = sys.argv[sys.argv.index("-o") + 1] if "-o" in sys.argv else inp + ".uf2"
    n_blocks, n_bytes = convert_bin_to_uf2(inp, outp)
    verify_uf2(outp, n_blocks)
    print(f"OK: {inp} ({n_bytes} bytes) -> {outp} "
          f"({n_blocks} blocks, {os.path.getsize(outp)} bytes, verified)")
