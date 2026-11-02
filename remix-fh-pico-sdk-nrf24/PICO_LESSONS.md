# PICO_LESSONS.md — RP2040 blade quirks, fixes & bench procedures

Applies to every RP2040 Pico blade on the FHAdvanced-REMIX Bus B (GPS, CC1101,
future radio blades). Read before debugging any Pico blade link problem.
Last verified on the bench: 2026-11-01 (GPS blade, hub <-> blade SPI proven
end-to-end: MODE 1 261-byte stream, CRC-valid GPS_FIX frames, RTC locked from
the blade, PICO=UP).

## 1. THE BIG ONE — RP2040 SSP slave in MODE 0 sends ONE byte per CS

**Symptom:** hub dumps show the first byte correct, then all `ff` (underrun).
Every pump/preload/SSE-toggle/reset variant fails identically.

**Cause (RP2040 datasheet / pico-examples issue #115):** with SPH=0 (CPHA=0)
the slave select *freezes the serial peripheral register* — the SSP slave
transmits exactly one byte per CS assertion. It cannot stream a frame with CS
held low.

**Fix:** run the blade bus in **MODE 1 (CPOL=0, CPHA=1)**.
- Blade: `spi_set_format(SPI_PORT, 8, SPI_CPOL_0, SPI_CPHA_1, SPI_MSB_FIRST)`
- Hub (`fh_hub_link.c`): Bus B devices `dc.mode = 1` (Bus A peers stay MODE 0)

**Rule: any new RP2040 blade starts in MODE 1. Never "fix" a one-byte-then-ff
underrun by changing the pump — the phase is the cause.**

## 2. Health/liveness — Pico blades do NOT heartbeat

`fh_health_on_heartbeat`/`fh_health_on_pong` were the only paths that marked a
node `seen`; a polled/fired Pico blade would read `PICO=dn` forever even while
its frames flowed. Fixed two ways (commit 048812a):

- `fh_health_on_frame(src_id)`: any validated frame proves liveness — marks
  seen, refreshes the beat clock, promotes UNKNOWN/HUNG -> HEALTHY. Wired into
  `route_frame` (skips src_id 0).
- Blade slot `src_id`: the GPS blade slot (not just the radio slot) carries
  FH_SRC_PICO (3), or its frames route with src_id 0 and are invisible to the
  health monitor.

**Rule: a new Pico blade must carry the right wire src_id in `s_blades[]`, and
any frame it emits counts as its liveness beat. HUNG->PING->PONG aging still
applies after the first frame.**

## 3. Simulated-GPS bench mode (indoor bring-up)

`FH_GPS_SIM=1` (CMake option, default OFF) bypasses uart1/NMEA and injects a
hardcoded valid fix (51.5 N, -0.12 W, 8 sats, HDOP 0.9) into the normal
`gps_stage_fix()` path. Use it when no sky view is available — it proves the
full encode -> SPI -> decode -> `fh_time_on_gps_fix` -> RTC path.

- Build: `cmake -S . -B build -DPICO_BOARD=pico -DFH_GPS_SIM=1`
- **Never ship a field rig with FH_GPS_SIM=1.**
- Plain build (real NEO-6M on GP4/GP5 @ 9600, then UBX bump to 38400/5 Hz):
  just omit the flag.

## 4. Build environment (toolchain path is NOT inherited)

After deleting or recreating `build/`, a bare `cmake -S . -B build` fails with
`Compiler 'arm-none-eabi-gcc' not found`. The toolchain lives in the Arduino
pqt-gcc package, and no host picotool exists (PICO_NO_PICOTOOL=1). Working
recipe:

```
export PICO_SDK_PATH=C:/Users/Admin/pico-sdk
export PICO_TOOLCHAIN_PATH="C:/Users/Admin/AppData/Local/Arduino15/packages/rp2040/tools/pqt-gcc/5.0.0-9576866"
export PATH="$PICO_TOOLCHAIN_PATH/bin:/c/Espressif/tools/python/Scripts:/c/Program Files/CMake/bin:$PATH"
cmake -S . -B build -DPICO_BOARD=pico [-DFH_GPS_SIM=1]
cmake --build build
"C:/Users/Admin/picotool-2.3.1/picotool.exe" uf2 convert \
    build/fh_pico_gps.bin build/fh_pico_gps_sim.uf2 --family rp2040
```

Notes:
- picotool-converted uf2 is the reliable flasher artifact (uf2conv.py also
  works but picotool is what produced the verified v15-v17 images).
- Sanity check a .bin that "won't boot": verify boot2 CRC-32/mpeg2 over bytes
  0..251 equals the word at offset 252.

## 5. Flashing over USB — `cp` to the RPI-RP2 drive is NOT reliable

A plain `cp file.uf2 /d/` from the MSYS shell can leave the write cached and
the Pico never flashes ("no reboot"). Reliable sequence:

1. Pico in BOOTSEL -> drive `D:` (RPI-RP2) appears.
2. `Copy-Item '...uf2' 'D:\' -Force` via PowerShell, wait 2 s.
3. Eject: `(New-Object -ComObject Shell.Application).NameSpace('D:').Self.InvokeVerb('Eject')`
4. The drive vanishing = the reboot happened. LED boot narration (5 fast
   blinks on the GPS blade = new firmware running) confirms it.

## 6. Bench COM5 conventions (hub side)

- `C5=dn XS3=dn PICO=dn/UP | RTC=boot/ok` — fh_ui status line, 1 Hz.
- `BENCH p1/p2:` hub logs the first 32 bytes of both SPI phases per poll;
  first `ff` in a dump = underrun point. Remove these logs before field use.
- Read COM5 at 921600, dtr/rts False, discard the first seconds of output.

## 7. Pin map (GPS blade, fh_pico_pins_sdk.h)

SPI0 slave: SCK=GP18, MOSI(hub->blade)=GP16, MISO(blade->hub)=GP19, CS=GP21
(valid spi0 CSn only: GP1/GP5/GP17/GP21). NEO-6M: GP4 -> GPS RX, GP5 <- GPS TX.
LED=GP25, IRQ-out=GP6, debug uart0 GP0/GP1 @115200 (off by default).
