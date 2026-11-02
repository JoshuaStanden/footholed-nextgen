# remix-MULTI-CHECK.md — the review framework for FootHoled-REMIX

> **Future Kiro + Joshua: run THIS on every artifact before calling it done.**
> Named the "multi-check" by Joshua (2026-10-25). It exists because a single
> casual "one last sweep" kept missing bugs that surfaced later — the rule is
> **iterate until a full pass finds nothing**, not "check once more."
>
> Standing constraint: Kiro does NOT compile/flash on this machine. So "does it
> compile" means **compile-correctness by reading** — trace every symbol, type,
> signature, include, and cross-component call to its definition; name the
> residual library/bench unknowns explicitly rather than pretend they are
> resolved. Never claim "this compiles" as fact.

## The five passes

**Pass 1 — Documentation.**
The per-part five-doc set — WIRING, README (for people), AGENTS (for AI/LLM
agents), CONTINUITY (resume point), FUTURE (planning) — plus any cross-cutting
top-level docs: accurate, consistent with the code, no stale or contradictory
claims, nothing that lies to a cold reader. (The retired BENCH_GUIDE/HEAD_LINK
docs were folded into WIRING/README/AGENTS during the 2026-11-01 reconcile.)

**Pass 2 — Peripheral code scan.**
A reasonable once-over; assumptions OK. Surface-level correctness, obvious
issues, naming, structure.

**Pass 3 — Deep scan / compile-correctness (NO assumptions).**
Independent review as if a compiler. Every symbol exists; every call matches its
header signature; every include is present; types/struct-sizes/field-orders are
right; cross-component contracts agree (who sends what, frame sizes, opcodes).
Catch every show-stopper reading can reach.

**Pass 4 — Meticulous deep dive (no butterfingers).**
Top-to-bottom, interactions + runtime behavior, not just compile. Trace the
consequences of each fix. Disconfirm your own suspicions honestly (don't report
a hazard you can't trace to ground). Check concurrency, IRQ-safety, library call
semantics against the vendor's real contract — not just signatures.

**Pass 5 — Iterate until clean.**
Repeat passes 3–4 (fixing as you go) until a full pass finds **ZERO** new issues.
Only then is the artifact done. A genuine judgment call or needed intent → stop
and ask; never guess.

## ★ The doc-reconcile rule (added 2026-10-25, Joshua's catch)
**Pass 1 is not finished until the code is final.** If passes 3–5 CHANGE the
code, the pass-1 docs are now potentially STALE — they described the old code.
So after any code change in 3–5, **RE-RUN pass 1 on the affected docs** before
declaring the artifact clean. A doc that describes the old behavior is exactly
the defect pass 1 exists to catch; shipping it is a pass-1 failure, not a
separate task.

Effective sequence per artifact:
```
1 Docs → 2 Peripheral → 3 Deep → 4 Meticulous → 5 Iterate(3–4) until clean
        → RE-RUN 1 on any doc the code changes touched → done
```

## What "a residual" looks like (acceptable to leave, if NAMED)
- A library API that can only be confirmed on the user's installed version
  (e.g. arduino-pico `SPISlave1` names — verified against the vendor reference,
  but the installed version is the bench check).
- A hardware behavior (RF_SW1 sense, Bus-A QIO dummy-cycle alignment, 5 GHz on
  new silicon, a timing gap being long enough).
- A datasheet/IOMUX fact (ESP32-P4 quad-SPI pin legality).
These are listed in the artifact's KNOWN RISKS + CONTINUITY "Open bench items",
never silently assumed resolved.

## What is NOT acceptable
- Claiming "compiles" without tracing it.
- Reporting a bug you talked yourself into without verifying (flag-then-retract).
- Leaving a doc describing code you changed.
- Stopping on "one more sweep" instead of iterating to an empty pass.

_This framework governs all FootHoled-REMIX review. It is itself subject to the
doc-reconcile rule: if the process changes, update this file._
