# remix-fh-pico-sdk-gps — FUTURE (planning)

Forward-looking notes. This blade is **done and proven** — there is no rewrite
owed for it. Its forward role is to stay the reference the rest of the rig leans
on.

## 1. GPS stays THE reference (no port owed)
The GPS blade is already native Pico SDK with the bench-proven SPI pump + MODE 1.
It is the pattern the three arduino-pico RADIO blades (cc1101, nrf24, sx1262)
will copy IF their `SPISlave1` transport fails the bench. The repo-level plan for
that is `../remix-PICO-SDK-PORT-PLAN.md`, and each radio blade's own `FUTURE.md`
carries its port steps. Nothing about those ports changes this blade.

Keep this blade's SPI pump, dual-core split, and MODE 1 intact — they are the
source the others copy. If they ever change here, re-verify on hardware (this
blade only earns changes from a bench, not from reading).

## 2. Possible forward items (only if a bench need appears)
- **Sats in the SYS_STATE report.** `fh_fix_t` on the hub carries no satellite
  count, so the hub's SYS_STATE reports `sats = 0` even on a valid fix. If the
  bench wants a sats number, the hub's `fh_hub_time` can cache it from
  `fh_time_on_gps_fix` — an additive hub-side change, **no wire change and no
  change to this blade.**
- **Faster-than-cadence GPS latency.** Decision already closed (hub
  `fh_hub_req.c` R2): no separate on-demand GPS REQ frame; the periodic poll
  already requests on demand. If a bench scenario ever needs sub-cadence latency,
  fire the existing GPS poll frame through the hub requester — no header change.
- **Geotag path.** The hub geotags capture rows from the cached fix; that logic
  lives on the hub, not here.

## 3. Not planned
- No Arduino version revival — the native SDK blade is the chosen, proven form.
- No on-device interpretation — the blade captures/stages; all interpretation
  lives where Medusa runs.
