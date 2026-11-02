# Releases — prebuilt GPS blade firmware

Binaries here are deliberately committed (gitignore `*.uf2` is overridden
for this folder only) so anyone can flash a Pico GPS blade without building
from source.

| File | Version | Flash with |
|---|---|---|
| `fh_pico_gps_v19_timefix.uf2` | v19 (current) — MODE 1 SPI link, real NMEA parse with RMC-time + longitude-degree fixes, `FH_GPS_SIM=0` | BOOTSEL -> copy to RPI-RP2 drive via PowerShell `Copy-Item` + eject |

Rebuild from source: `cmake -S . -B build -DPICO_BOARD=pico -DFH_GPS_SIM=0`
(see PICO_LESSONS.md for the full toolchain recipe). Do not commit newer
binaries elsewhere in the tree.
