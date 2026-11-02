# archive/arduino-blades — superseded arduino-pico radio blades

These are the original **arduino-pico** radio blades, kept here (not deleted)
because they were superseded by the native **pico-sdk** ports but the committed
project rule is: *the arduino blade stays until its SDK port is bench-proven,
then retires* — it is the fallback if an SDK port hits a wall on real silicon.

| archived (arduino) | replaced by (pico-sdk) | status |
|---|---|---|
| `remix-fh-pico-cc1101` | `../../remix-fh-pico-sdk-cc1101` | SDK port written + multi-checked, NOT yet bench-proven |
| `remix-fh-pico-nrf24`  | `../../remix-fh-pico-sdk-nrf24`  | SDK port written + multi-checked, NOT yet bench-proven |
| `remix-fh-pico-sx1262` | `../../remix-fh-pico-sdk-sx1262` | SDK port written + multi-checked, NOT yet bench-proven |

(The GPS blade was already native SDK — `../../remix-fh-pico-sdk-gps` — and had
no arduino version to archive.)

## When is it safe to delete these?
Once the corresponding **SDK blade is bench-proven on your hardware** (radio
answers, the hub link carries CRC-valid frames, the verbs work), its archived
arduino counterpart can be deleted. Until then, keep it: if the SDK port fails
on silicon in a way reading can't catch, this is the known-shape fallback.

## Why they were replaced
The arduino-pico `SPISlave` transport could not reliably stream as a slave on
the RP2040 (the one-byte-per-CS behavior + the callback model). The GPS blade
was rewritten to the native pico-sdk with a hand-written full-duplex pump and is
bench-proven; the three radio blades followed that pattern. See
`../../remix-PICO-SDK-PORT-PLAN.md`.

Moved here 2026-11-01. These are reference/fallback only — do not develop them
further; all active work is in the `remix-fh-pico-sdk-*` blades.

**Note on stale internal links:** each archived blade's own docs
(its `AGENTS.md`, `CONTINUITY.md`, `FUTURE.md`, `WIRING.md`) still contain
relative links written for the old repo-root location (`../AGENTS.md`,
`../remix-MULTI-CHECK.md`, `../remix-fh-pico-sdk-gps/...`). After the move into
`archive/arduino-blades/<blade>/` those resolve one level too high and are
therefore broken — **stale by design, not worth fixing on retired files.** The
live equivalents are the repo-root `AGENTS.md` / `remix-MULTI-CHECK.md` and the
`remix-fh-pico-sdk-*` blades; this README's own links (above) use the correct
`../../` depth. When a blade is deleted (per the rule above) its broken links go
with it.
