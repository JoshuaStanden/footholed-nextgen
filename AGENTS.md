# AGENTS.md — critical constraints for AI assistants working on this rig

Read this BEFORE touching any Pico (RP2040) blade or the hub's Bus B link
layer. These rules were learned by bench debugging, not guessed. The full
source of truth is `remix-fh-pico-sdk-gps/PICO_LESSONS.md`; this file carries
the non-negotiables that every future agent and contributor must obey.

## 1. RP2040 SPI slave MUST be MODE 1 (CPOL=0, CPHA=1)

The RP2040 SSP slave transmits exactly ONE byte per CS assertion in MODE 0 —
the slave select *freezes the shift register* (RP2040 datasheet; also
pico-examples issue #115). Symptom: the hub receives byte 0 correct, then all
`ff` (underrun). NO pump/preload/SSE-toggle/FIFO-reset change fixes this.

- Blade firmware: `spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST)`
- Hub (`remix-FH-P4-hub-firmware/main/fh_hub_link.c`): Bus B devices `dc.mode = 1`.
  Bus A peers (ESP32-family, not RP2040) stay MODE 0.

If a future Pico blade shows the one-byte-then-underrun signature, do NOT
rewrite the SPI pump — check the SPI mode first.

## 2. Pico blades do NOT send heartbeats — liveness = frames

Pico blades are polled/fired; they never emit FH_TYPE_HEARTBEAT. The hub's
health monitor therefore proves their liveness from any validated frame via
`fh_health_on_frame(src_id)` (wired into `route_frame` in fh_hub_link.c).
The existing HUNG → PING → PONG aging still applies after the first frame.

Related trap: every Pico blade slot in `s_blades[]` must carry the correct
wire `src_id` (both the GPS slot and the radio slot carry FH_SRC_PICO = 3).
A slot with `src_id = 0` is invisible to the health monitor even while its
frames flow perfectly — it will read `dn` forever.

## 3. Building & flashing Pico blades (does not "just work" from a clean tree)

Build recipe (deleting `build/` wipes the cached toolchain path):

```bash
export PICO_SDK_PATH=C:/Users/Admin/pico-sdk
export PICO_TOOLCHAIN_PATH="C:/Users/Admin/AppData/Local/Arduino15/packages/rp2040/tools/pqt-gcc/5.0.0-9576866"
export PATH="$PICO_TOOLCHAIN_PATH/bin:/c/mingw64/bin:/c/Program Files/CMake/bin:$PATH"
cmake -S . -B build -DPICO_BOARD=pico     # add -DFH_GPS_SIM=1 for indoor bench only
cmake --build build
"C:/Users/Admin/picotool-prebuilt/bin/picotool.exe" uf2 convert build/<x>.bin build/<x>.uf2 --family rp2040
```

Flash reliability: a bare `cp file.uf2 /d/` to the RPI-RP2 drive does NOT
flash (cached write → "no reboot"). Use PowerShell `Copy-Item … 'D:\' -Force`
then Shell.Application eject; the drive vanishing = the reboot happened.
GPS blade boots with 5 fast LED blinks (bench boot narration).

`FH_GPS_SIM=1` injects a hardcoded valid GPS fix for indoor bench bring-up —
NEVER ship a field rig with it enabled.

## 4. Verified-working reference implementation

`remix-fh-pico-sdk-gps/` is the bench-proven Pico blade (2026-11-01):
MODE 1, 261-byte transaction streaming, CRC-valid GPS_FIX frames, hub RTC
locked from a blade frame, `PICO=UP`. When writing a NEW Pico blade (cc1101 /
nrf24 / sx1262 SDK ports), copy its SPI setup, frame staging and pump design
from there instead of reinventing. See also `remix-MULTI-CHECK.md` for the
multi-agent verification workflow.

**COMMITTED POLICY (2026-11-01): ALL Pico blades port to the native pico-sdk.**
cc1101, nrf24, and sx1262 are being rewritten off arduino-pico `SPISlave1` onto
the GPS blade's SDK pump — a decided plan, not conditional on a bench failure.
Order: cc1101 (pilot) → nrf24 → sx1262 (hardest shim). The arduino version of
each stays until its SDK replacement is bench-proven, then is retired. Full plan:
`remix-PICO-SDK-PORT-PLAN.md`; per-blade steps in each blade's `FUTURE.md`.

## 5. Pin map (GPS blade — other Pico blades keep the same Bus B assignment)

SPI0 slave: SCK=GP18, MOSI(hub→blade)=GP16, MISO(blade→hub)=GP19, CS=GP21
(valid spi0 CSn only: GP1/GP5/GP17/GP21). NEO-6M: GP4→GPS RX, GP5←GPS TX
@ 9600, then UBX bump to 38400/5 Hz (best-effort; NMEA parses at 1 Hz).
LED=GP25, IRQ-out=GP6, debug uart0 GP0/GP1 @ 115200 (off by default).
