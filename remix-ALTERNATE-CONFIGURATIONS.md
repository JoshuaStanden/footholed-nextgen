# remix-ALTERNATE-CONFIGURATIONS.md — head/display configuration options

**Status: THEORY / options menu, nothing built.** This file records the viable
head (face) configurations so the project can serve different budgets and
hardware-on-hand, instead of assuming everyone buys the 8.8" DSI panel. The
primary/endgame plan lives in `remix-HEAD-ROADMAP.md`; this file is the menu of
alternatives around it. The UI *model* (layered shell, CON/RECON/ANALYZE, the
status strip, capture rail) is shared across all configs — see the UI MODEL
section of `remix-HEAD-ROADMAP.md`. These configs differ only in WHICH GLASS
shows WHAT.

## The configuration spectrum (cheapest hardware-on-hand → endgame)

### Config A — 3.5" + CYD (BUDGET TIER, "use what you already have")
**For people who already own a Waveshare ESP32-S3-Touch-LCD-3.5 and a Cheap
Yellow Display (CYD, ESP32-2432S028), and don't want to spend ~$60 on the 8.8".**

- **Waveshare 3.5" (its onboard S3) = the WORK SURFACE.** Full height, NO status
  strip: rail (CON/BT/2.4/5/NRF/SUB/⚙) + content pane + ✕ back-stack + the
  RECON/ANALYZE mode button + LoRa stick *management* (under ⚙). Everything you
  TOUCH lives here. Dropping the strip gives CON the taller feed the 3.5" was
  short on.
- **CYD (its own ESP32) = the STATUS PANEL (glance-only output).** Renders the
  health pairs (C6/BT, S3/2.4, C5/5), GPS+sats, LoRa link state, link, AND the
  clock/date that never fit on the 3.5" strip. It has a whole 320×240 to itself,
  so it shows MORE status than a thin strip ever could, with room for the
  date+time that was set aside.
- **Topology: CYD is SUBTENDED TO THE S3 HEAD, not the P4.** Why: a status
  display hung off the P4 as a Bus-A peer would cost 2 scarce P4 pins (CS+IRQ) on
  an already-stretched budget. The S3 has spare GPIO and masters a SECOND local
  SPI bus to the CYD — **zero P4 pin cost.** (This pin argument is the whole
  reason for the topology; do not "simplify" it to a hub peer.)
- **Data flow: ONE source of truth (the P4 hub).** P4 → full feed → S3. The S3
  renders the fields it owns (work surface) and FORWARDS the status subset as
  RAW VALUES to the CYD; the CYD's own ESP32 renders its own status screen from
  those values. The CYD is a SMART PEER fed a data subset — NOT a dumb pixel
  slave the S3 draws into (both have real MCUs; each renders its own glass).
  Keep the handoff as raw values (not formatted strings) so adding a status
  field upstream is a one-value passthrough change, and the CYD decides whether
  to show it.
- **Discipline that keeps it clean:**
  - No duplication — the status lives on the CYD, so the Waveshare does NOT also
    draw health dots. One datum, one screen.
  - Split by interactivity — **CYD = everything glanceable (passive output);
    Waveshare = everything you tap (rail, content, mode button, LoRa manage).**
    The CYD never takes touch input and never parses protocol.
  - Status is slow-changing chrome (health/GPS/clock), so the S3→CYD forward
    cadence doesn't need to be fast. Do NOT put fast-moving capture data on the
    CYD — lag would show there; capture feeds stay on the Waveshare.
  - **The CYD's RESISTIVE touch (stylus-only) ENFORCES output-only — a feature,
    not a limit.** Resistive input needs a plastic stylus and is miserable for
    finger taps, so no one will ever want to punch input on the CYD. That makes
    the passive/interactive split PERMANENT and self-enforcing: the Waveshare
    (capacitive) owns ALL touch forever; the CYD is glance-only output and can
    never erode into holding a control. Design to this: never put an interactive
    element on the CYD even if space tempts it.
- **New wiring/code needed:** a local S3→CYD SPI link (S3 master, CYD panel) +
  a tiny "status fields" passthrough on the S3 and a status-render firmware on
  the CYD. NO P4 involvement, NO frozen-protocol change. (The raw-values handoff
  lets the CYD DISPLAY a new status field with no wiring change — but it stays
  output-only; the handoff never carries input the other way.)
- **Honest limits:** two physical screens to glance between; the CYD is a weak
  panel (2.8" 320×240 resistive, ILI9341). Fine for passive status; not a work
  surface — and, per above, that resistive panel is exactly why it stays passive.

### Config B — 3.5" solo (MINIMAL, one screen)
The current `remix-FH-WS-head-firmware/` as-is: the 3.5" shows the thin status
strip + rail + content on one cramped screen. No CYD, no 8.8". The strip is tight
(that crowding is what drove Configs A and C). This is the baseline that exists
today.

### Config C — 8.8" DSI on a 2nd ESP32-P4 (ENDGAME)
The primary plan — see `remix-HEAD-ROADMAP.md`. 480×1920 has the vertical room to
hold a persistent status strip AND a full content pane on ONE screen, so it needs
neither a cramped strip (Config B) nor a second display (Config A). Costs ~$60 for
the panel + a second P4. This is where the project is heading; A and B are for
people not buying that yet.

## Why keep all three
The UI model is identical across them (same rail, same modes, same data feed from
the hub), so supporting multiple configs is mostly a RENDERING-TARGET choice, not
three different UIs. Config A specifically lets an existing owner (3.5" + CYD, no
new spend) get the uncramped layout that Config C buys with money. That is the
point of writing it down: the architecture already separates "hub owns data" from
"face renders it," so pointing the render at 1, 2, or a bigger piece of glass is a
configuration, not a rewrite.

## Status / next
THEORY ONLY. Nothing here is built; the working head today is Config B
(`remix-FH-WS-head-firmware/`). If Config A is pursued, it is an S3-ERA
enhancement (the 8.8"/Config C does not need it). The shared UI model it renders
is specced in `remix-HEAD-ROADMAP.md` (UI MODEL section).
