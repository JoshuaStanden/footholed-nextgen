# remix-BUILD-TOOLCHAIN.md — how to build the Pico blades (the RIGHT way)

> **Audience: the build/toolchain AI (or any human) that compiles and flashes
> on the real hardware.** This repo is written on a machine with NO toolchain;
> all building and flashing happens on the owner's Windows box. This doc encodes
> the PROVEN path that already built the bench-working GPS blade, so you do not
> repeat the picotool/MinGW rabbit hole that derailed earlier attempts.
>
> **TL;DR — never BUILD picotool; use the PREBUILT one.** Every blade sets
> `PICO_NO_PICOTOOL=1` so the SDK build never touches picotool, and the
> `.bin` → `.uf2` step uses either the bundled `uf2conv.py` or the prebuilt
> `picotool.exe` (both produce valid UF2; see §9). If you find yourself
> installing MinGW/libUSB to make picotool COMPILE, STOP — you are on the
> wrong path. The prebuilt binary already exists and works.

---

## 1. The pipeline (what actually happens)

Source `.c/.cpp`  →  **cross-compiler** (ARM GCC) under **CMake + Ninja**  →
`.elf` + `.bin`  →  **`uf2conv.py`**  →  `.uf2`  →  drag onto the Pico's
BOOTSEL drive.

Four tools, nothing more:
1. **ARM cross-compiler** `arm-none-eabi-gcc` — makes RP2040 (ARM) machine code.
   This repo reuses the one that ships inside the Arduino rp2040 core (see the
   `PICO_TOOLCHAIN_PATH` below) so there is nothing extra to install if the
   owner already has the Arduino Pico core.
2. **Pico SDK** — Raspberry Pi's RP2040 hardware library. `PICO_SDK_PATH` points
   at it.
3. **CMake + Ninja** — read `CMakeLists.txt`, run the compiler, link the `.elf`.
4. **`uf2conv.py`** (bundled in each blade) — converts the `.bin` to `.uf2`.
   Standard UF2: 512-byte blocks, RP2040 family id `0xe48bff56`. Pure Python,
   no compiler needed.

**Why never BUILD picotool:** picotool must be COMPILED with a NATIVE (host)
compiler + libUSB. On Windows that means MinGW-w64 + libUSB, which is exactly
what failed before. `PICO_NO_PICOTOOL=1` tells the SDK "skip picotool, still
emit elf/bin/hex". The `.uf2` comes from `uf2conv.py` (bundled) OR the prebuilt
`picotool.exe` (§9) — no host compile ever required.

---

## 2. Prerequisites (install once)

- **Pico SDK** cloned somewhere, e.g. `C:/Users/Admin/pico-sdk`.
- **Arduino rp2040 core** (Earle Philhower) installed — it brings the ARM GCC
  toolchain this repo points at. Path looks like
  `.../Arduino15/packages/rp2040/tools/pqt-gcc/<version>`.
- **CMake** (`C:/Program Files/CMake/bin/cmake.exe`).
- **Ninja** on PATH (the Arduino core also ships one; or install standalone).
- **Python 3** on PATH (for `uf2conv.py`).
- **git-bash** (the commands below use bash syntax on Windows).

No MinGW. No libUSB. No picotool.

---

## 3. Build a blade (the proven recipe)

This is the exact sequence that built the bench-working GPS blade, generalized.
Replace `<blade-dir>` and `<target>` per the table in §5.

```bash
# point at the SDK and the ARM toolchain (adjust versions/paths to the box)
export PICO_SDK_PATH="C:/Users/Admin/pico-sdk"
export PICO_TOOLCHAIN_PATH="/c/Users/Admin/AppData/Local/Arduino15/packages/rp2040/tools/pqt-gcc/5.0.0-9576866"
export PATH="$PICO_TOOLCHAIN_PATH/bin:$PATH"

cd "/c/Users/Admin/Desktop/FootHoled-NextGen/<blade-dir>"

"C:/Program Files/CMake/bin/cmake.exe" -B build -S . \
  -DPICO_BOARD=pico -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -G Ninja

"C:/Program Files/CMake/bin/cmake.exe" --build build

python uf2conv.py build/<target>.bin -o build/<target>.uf2
```

Flag notes:
- `-G Ninja` — use Ninja as the builder (fast, reliable here).
- `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` — quiets a CMake policy error seen with
  this SDK/CMake combo.
- `PICO_NO_PICOTOOL=1` is set INSIDE each `CMakeLists.txt`, not on the command
  line — you do not pass it.

---

## 4. Flash (Windows, BOOTSEL drag)

**Use `tools\flash_pico.ps1` — it automates this AND proves the UF2 was
recognized: after the copy (still in BOOTSEL) it runs `picotool verify`, which
reads the device's flash back over USB and compares it byte-for-byte against
the file. Copy+eject alone only proves the write was flushed — verify is the
recognition proof, and the drive vanishing is only the reboot proof:**

```powershell
powershell -ExecutionPolicy Bypass -File tools\flash_pico.ps1 build\<x>.uf2
powershell -ExecutionPolicy Bypass -File tools\flash_pico.ps1 build\<x>.uf2 -Picotool   # USB flash + verify
```

Manual steps (what the script does, for reference):
1. Hold **BOOTSEL** on the Pico while plugging in USB. It mounts as a drive
   named `RPI-RP2`.
2. Copy the `.uf2` onto it with PowerShell `Copy-Item` and then eject:
   a bare `cp` in git-bash does NOT reliably flash — use `Copy-Item` + eject.
3. The Pico reboots into the new firmware — the drive vanishing IS the reboot
   proof; if it is still mounted, the flash did not take. **Boot-blink
   signatures** tell you it is running (see each blade's code/docs).

---

## 5. Per-blade specifics

| Blade dir | CMake target | `.bin` / `.uf2` name | Extra |
|---|---|---|---|
| `remix-fh-pico-sdk-gps` | `fh_pico_gps` | `fh_pico_gps` | `-DFH_GPS_SIM=1` for indoor bench; `-DFH_DEBUG_UART=1` |
| `remix-fh-pico-sdk-cc1101` | `fh_pico_cc1101` | `fh_pico_cc1101` | `-DFH_DEBUG_UART=1` optional |
| `remix-fh-pico-sdk-sx1262` | `fh_pico_sx1262` | `fh_pico_sx1262` | **needs RadioLib — see §6** |
| `remix-fh-pico-sdk-nrf24` | `fh_pico_nrf24` | `fh_pico_nrf24` | none — all C, in-tree driver, no external deps |

All blades use the SAME toolchain, the SAME `PICO_NO_PICOTOOL=1` + `uf2conv.py`
path. Only **sx1262** has an extra dependency (RadioLib); gps/cc1101/nrf24 have
none.

---

## 6. sx1262 ONLY — the RadioLib dependency

The LoRa blade links the upstream **RadioLib** library (its official Pico-SDK
HAL). It is NOT vendored in the repo; fetch it and PIN the tag (one-time):

```bash
git clone https://github.com/jgromes/RadioLib.git
cd RadioLib && git checkout 7.8.1 && cd ..
```

**Use 7.8.1 — this is a HARD requirement.** The shim calls the positional
`SX1262::begin(freq,bw,sf,...)` overload, which is deprecated-but-present in 7.x
and REMOVED on RadioLib 8.x/master. An 8.x build trips a deliberate `#error` in
`sx_shim.cpp`. Then add the path to the cmake line for this blade only:

```bash
"C:/Program Files/CMake/bin/cmake.exe" -B build -S . \
  -DPICO_BOARD=pico -DCMAKE_BUILD_TYPE=Release \
  -DRADIOLIB_PATH="C:/path/to/RadioLib" \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -G Ninja
```

The build adds RadioLib as a subdirectory, defines `RADIOLIB_BUILD_RPI_PICO`,
and compiles RadioLib's `src/hal/RPiPico/PicoHal.cpp`. Nothing else changes.

---

## 7. If a build fails — triage in THIS order

1. **"picotool" / libUSB / MinGW errors** → you are on the wrong path. Confirm
   the blade's `CMakeLists.txt` has `set(PICO_NO_PICOTOOL 1)` (it does) and that
   you are NOT trying to build picotool. Do not install MinGW/libUSB.
2. **`arm-none-eabi-gcc: not found`** → `PICO_TOOLCHAIN_PATH` is wrong or not on
   PATH. Fix the export in §3.
3. **`PICO_SDK_PATH` errors / `pico_sdk_import.cmake` not found** → the SDK path
   is wrong.
4. **sx1262: `#error ... RadioLib ... begin()`** → wrong RadioLib version. Check
   out 7.8.1 (§6).
5. **sx1262: `Set -DRADIOLIB_PATH=...` FATAL_ERROR** → you forgot the
   `-DRADIOLIB_PATH` flag (§6).
6. **CMake policy error** → ensure `-DCMAKE_POLICY_VERSION_MINIMUM=3.5` is on the
   configure line.
7. **Flashes but does nothing / wrong boot blinks** → that is a hardware/wiring
   or firmware-logic issue, NOT a toolchain issue — stop here and report the
   blink pattern + behavior; do not change the build.

Report the EXACT error text. Do not "fix" it by switching toolchains, vendoring
picotool, or editing a blade's `CMakeLists.txt` — those are the moves that lost
control last time. The recipe above is known-good; a failure is almost always a
path/version mismatch in §2–§3, not a reason to re-architect the build.

---

## 8. What NOT to do (hard rules for the build AI)

- **Do NOT install or compile picotool.** `uf2conv.py` replaces it here.
- **Do NOT install MinGW-w64 or libUSB** for this project's sake.
- **Do NOT edit any blade's `CMakeLists.txt`** to "make the build work" — the
  CMake is correct; fix the environment instead. (A genuine CMake bug is a
  report-back item, not a silent edit.)
- **Do NOT bump RadioLib off 7.8.1** without porting `sx_shim.cpp`'s `begin()`
  to the `ConfigLoRa_t` form first.
- **Do NOT change blade source to chase a build error** — build errors are
  toolchain/env issues; source changes are a separate, reviewed activity
  (see `remix-MULTI-CHECK.md` / each blade's `AGENTS.md`).

---

## 9. Host tools on this box — picotool (prebuilt) and mingw64 (roles)

### picotool — PREBUILT, at `C:/Users/Admin/picotool-2.3.1/`

- **Source of the binary:** the OFFICIAL prebuilt from
  `raspberrypi/pico-sdk-tools` release `v2.3.1-0`
  (`picotool-2.3.1-x64-win.zip`, extracted 2026-11-02). Never compile it.
  An older `C:/Users/Admin/picotool-prebuilt/` (v2.0.0) also exists and is
  proven on real images — 2.3.1's `uf2 convert` output is byte-identical to
  it (verified 2026-11-02 on a test image), so either works; default to 2.3.1.
- **USB support is live:** `picotool info` with nothing connected correctly
  reports "No accessible RP-series devices in BOOTSEL mode" — libusb is
  statically included in the prebuilt; no libUSB install, no WinUSB driver
  install needed for BOOTSEL-mode devices.
- **What it is FOR here:**
  - `picotool uf2 convert build/<x>.bin build/<x>.uf2 --family rp2040` —
    the `.bin` → `.uf2` step in the blade recipe (AGENTS.md §3). Produces the
    same artifact uf2conv.py does (verified byte-identical between picotool
    2.0.0 and 2.3.1; uf2conv.py output differs in block layout but is also a
    valid UF2 and is what historically flashed the GPS blade — both are fine).
  - `picotool load build/<x>.uf2` — flash over USB while the Pico is in
    BOOTSEL mode. An ALTERNATIVE to the BOOTSEL drag+eject dance (§4), not a
    replacement policy: if `load` misbehaves on Windows, fall back to the
    proven `Copy-Item` + eject method.
  - `picotool info` / `picotool verify` / `picotool save` / `picotool reboot
    -u` — inspect/verify/backup/reboot devices. `reboot -u` is useful to kick
    a running blade back into BOOTSEL without touching the board.
- **What it is NOT for:** the blade BUILD. The SDK never runs picotool
  (`PICO_NO_PICOTOOL=1` stays set in every CMakeLists).

### `C:\mingw64` — a HOST-ONLY toolchain. Three rules.

It is a full native Windows toolchain (GNU 16.2 / clang, cmake, ninja,
mingw32-make) — the same toolchain family Raspberry Pi's official picotool
prebuilt is compiled with. That is its proper universe: HOST programs. It is
NOT part of the blade build.

1. **Never on the blade-build PATH.** The blade recipe's PATH carries ONLY
   the ARM cross-compiler (`PICO_TOOLCHAIN_PATH/bin`), ninja
   (`/c/Espressif/tools/python/Scripts`), and CMake
   (`/c/Program Files/CMake/bin`). The old `/c/mingw64/bin` entry was
   REMOVED from AGENTS.md §3 and all eight PICO_LESSONS.md copies
   (2026-11-02) — it was a leftover from the picotool rabbit hole and could
   shadow the ARM toolchain with a host-only gcc if PATH order shifted.
2. **Never compile picotool with it.** That is the rabbit hole that lost
   control last time; the prebuilt (above) replaced it permanently.
3. **Legitimate uses:** building/debugging HOST-side tools (a Windows utility
   for bench use, a Python C-extension, etc.) is fine. Anything that runs on
   the laptop may use mingw64; anything that runs on a Pico uses pqt-gcc +
   the Pico SDK, and anything that runs on an ESP32 uses ESP-IDF (separate,
   untouched).

### ESP-IDF (ESP32 boards) — deliberately separate
The ESP32-P4 hub and XIAO-S3/C5/head peers build ONLY through ESP-IDF
(`idf.py`, its own venv + toolchain, COM5 @ 921600). None of the mingw64 or
Pico toolchain above participates in an ESP-IDF build, and no ESP-IDF tool
leaks into the Pico build. Three build universes, three disjoint toolchains:
**pqt-gcc+Pico SDK → Pico blades · ESP-IDF → ESP32 boards · mingw64 → host
tools only.**
