# remix-3D-PRINTING.md — enclosure / 3D-print planning (TEXT ONLY)

**Status: THEORY / planning. Nothing generated yet.**

## Why this file is text-only (read before adding anything)
The original FootHoled repo got ENCUMBERED by carrying 3D renders + STL binaries
through every design iteration — they bloated the repo and made every
upload/download heavier, even when the files were useful. We are NOT repeating
that. RULES for this project's 3D work:
- **This doc is text only.** Decisions, dimensions, material plan, workflow.
- **When geometry is generated, the `.scad` OpenSCAD SOURCE is the artifact** —
  it is text, diffs cleanly, and stays light. It lives in the repo.
- **STL / render / mesh BINARIES stay OUT of version control.** They are
  generated on demand from the `.scad` (render → export → slice) and discarded.
  If a binary must be shared, hand it over directly, don't commit it.
- Keep it light so iteration never becomes the burden it was last time.

## Form factor: cyberdeck (hard-angle, boxy — NOT sculpted)
Cyberdecks are obtuse/hard-angled by nature, which maps perfectly onto OpenSCAD's
constructive-solid-geometry strengths (prisms, planes, chamfers, standoffs). No
organic/compound-curve sculpting is in this design's vocabulary — even the
"ergonomic" handles are just knurled/filleted TUBES (parametric, easy). So the
usual CAD-by-text weakness (organic shapes) does not apply here.

### Layout — a solid slab cyberdeck (front face / back bank / depth)
One rigid enclosure, not separate modules. Think of it in three layers
front-to-back, with the antennas exiting the top.

**FRONT FACE (operator's view), left → right:**
```
[ batt ][  W-S  (landscape) ][ CYD (portrait) ][ batt ]
```
- **Battery stacks BOOKEND the face** — the two 18650 stacks are the left/right
  end columns (the grips), FRAMING the screens, not behind them. (Pack/BMS/power
  topology is the owner's EXTERNAL plan, deliberately NOT specified in these docs
  — only the tube/stack GEOMETRY is in scope.)
- **Waveshare 3.5" — LANDSCAPE, left of center.** Capacitive WORK surface (all
  touch). Landscape fits the work UI (rail + wide content pane).
- **CYD — PORTRAIT, right of center.** Glance-only STATUS panel (resistive,
  output-only; see `remix-ALTERNATE-CONFIGURATIONS.md` Config A). Portrait is the
  natural shape for a stacked status column (health dots / clock / GPS / LoRa
  read top-to-bottom like an instrument panel).
- **Gap between the two screens:** wide enough to open up interior room behind
  the face, NARROW enough that the eye doesn't have to travel to check status
  (CYD stays in peripheral vision while working the Waveshare). Exact width is a
  bench/ergonomics call.

**BACK BANK, left → right (makes the unit THICK but one solid slab):**
```
[ L | L | L | L | L ]   ← N blades across the width; N is flexible (5, 6, …)
```
- Each **blade = a perf board** (Pico + radio stacked, or two XIAOs stacked,
  etc.), enclosed, standing as a vertical card behind the display layer.
- **Count is PARAMETRIC — N-wide, set by the radio set at build time.** Leaves
  room for the parked card-reader radios / spare slot to return by widening the
  bank one slot, not rearchitecting the case.
- **Blades are NOT guaranteed uniform.** Owner standardizes on perf-board stock
  to get CLOSE, but a board may be wider (antenna-routing room) or taller than
  its neighbor. Plan: **space them uniformly left→right and BOTTOM-ALIGN them**;
  accept a "close" fit, not a perfect one.

**DEPTH:** batteries + screens (front) and blade bank (rear) all live in one
rigid slab. Thicker than a single-layer deck, but one unified cyberdeck unit —
the thickness is what buys the RF separation and the rigidity.

### Antennas — SMA pigtails to a clean uniform TOP bulkhead row
The blades bottom-align, so their TOPS are ragged (different board heights). The
fix that makes this a non-issue:
- **Run an SMA-to-SMA pigtail from each board up to a single straight TOP face**
  where panel-mount SMA BULKHEAD connectors sit in an even row. The ragged board
  heights hide INSIDE; the top antenna-exit face is uniform and intentional.
- This also makes the top face parametric-simple (evenly-spaced bulkhead holes on
  one straight edge, not N holes at N heights), and **decouples antenna POSITION
  from board position** — so antennas can be placed for SEPARATION independent of
  how the boards pack.
- **Bulkhead SMA is the right field-device choice anyway:** it takes the whip's
  mechanical load off the hand-soldered board (board stays happy, pigtail flexes,
  bulkhead takes the abuse).
- **GPS ceramic patch wants CLEAR SKY (faces UP).** On the top face, the patch
  gets its sky view for free; whips mount vertical. Same top region, two mounting
  attitudes (flat-up pad for the patch, vertical for whips).
- **Interior cost to budget:** pigtail slack needs a little bend radius / tuck
  room (part of why the slab is thick; thin coax like RG178 bends tight and
  forgives). Route antenna coax away from P4 / display-ribbon noise for the
  noise-sensitive radios — a free win while placing them.

### Parametric structure this implies (for the eventual `.scad`)
- The blade bank is driven by a **per-blade table**, not one repeated size:
  roughly `[{width, height, antenna_x_offset, antenna_type}, …]`, packed
  left→right with a configurable gap. **Case width DERIVES from the sum** of
  blade widths + gaps (don't hardcode it).
- Each blade's top bulkhead position comes from its slot, not its board height
  (pigtails absorb the difference), so the top row stays uniform.
- **Open question for later (not now):** blade RETENTION with varied heights —
  individual slots/brackets per blade vs. one shared lid (a single flat lid is
  awkward over varied heights; likely per-blade retention or a stepped/padded
  lid). Also: is the back a REMOVABLE service panel (swap a failed blade) or
  sealed?

## Multi-part print breakdown (LOCKED — two independent structures + sides)
Decompose by FUNCTIONAL JOB, ~6-8 printed parts. Shared DNA across both
structures: **the PLATE does the work (structure + holds components); the COVER
is a simple bolt-on lid edge-screwed to the plate.** Front and back are the same
idea facing opposite directions; the interior between the two plates holds the
wiring that joins them. Batteries (the two side holders) are specced separately.

### Structure 1 — FRONT (display) assembly
- **Screen plate** = the structural member. Screens are held on **PRINTED
  STANDOFFS that rise from the plate** at each display's hole pattern (**M2** for
  the Waveshare 72×48.5 rectangle, **M3** for the CYD). Screws go from the BACK of
  the plate, through the boss, into the back of each screen. No drilling — the
  hole pattern is just geometry per panel.
- **Standoff HEIGHTS are tuned PER DISPLAY so both screen faces sit COPLANAR** at
  the front. The plate stays flat; the differing standoff heights absorb the
  depth mismatch between the two panels. The dimension that governs coplanarity
  is each panel's **glass-face → mounting-hole seating-plane** distance (NOT just
  board depth) — measure that per display (added to the gather list).
- **Front cover** = one piece over BOTH screens: two apertures (Waveshare
  offset/R6 + CYD) + button/port cutouts + a solid band across the **~½"–1"
  (~13-25 mm, start ~15-20) gap** between them. Fastened by **edge screws into
  the screen plate.** Just a protective lid — no glass-trapping.

### Structure 2 — BACK (blade) assembly ("build it as we fly it")
- **Blade back plate** = a flat, REUSABLE MOUNTING FIELD — a **screw-hole grid**
  (pegboard-style, e.g. regular M3 positions) rather than a plate custom-cut to
  each blade. Decouples the plate from the blades: a blade changing size never
  reprints the plate.
- **Blade frames / boxes** = a small LIBRARY of printed frames in **a few sizes**
  (small / medium / wide-for-antenna-routing), reused across blades, each holding
  a blade or a board (P4, upper + lower XIAOs, the blades). Frames **screw into
  the back-plate grid** anywhere they need to sit. Each frame carries its SMA
  **pigtail routing** channel/exit toward the top bulkhead row. Grid pitch should
  suit the perf-board standardization so frame feet land cleanly.
- **Shallow back:** frames only as tall as the board needs; cover close behind —
  keep the slab from getting needlessly deep.
- **Back cover** = the lid, **edge-screwed into the back plate**, closing the
  blade enclosure. (Grid + swappable frames make the back serviceable by design:
  a dead blade = unscrew one frame, not surgery on the whole back.)

### Structure 3 — SIDE 18650 HOLDERS (×2, L+R) = the structural spine
- The two side holders bookend the front face (`[batt][W-S][CYD][batt]`) AND tie
  the whole slab together: each holder lands **heat-set brass inserts in BOTH the
  screen back plate (front structure) AND the blade back plate (back structure)**,
  with **several inserts per plate.** So a holder is not a cantilever off one
  edge — it is the **left/right wall that clamps front + back into one rigid
  slab**, with the grip load spread across inserts in two plates. (Use heat-set
  brass inserts, not screws threaded into bare plastic — the holders are opened
  repeatedly to swap/charge cells and bare-plastic threads strip; inserts hold in
  pull-out. Add a registration lip/tongue at the holder↔plate seam so the plastic
  joint takes the grip SHEAR and the screws just hold it closed.)
- **Vented, with inter-cell spacing — for LONGEVITY + off-gas, NOT heat dumping.**
  18650s here run warm, never untouchably hot (hot = a fault a BMS handles, which
  is the owner's external plan). Vents/slots + an air gap between the two cells
  per side keep the cells at ambient instead of trapped-warm next to the P4/DC-DC
  heat (warm cells age faster), and give a failing cell's off-gas somewhere to go
  instead of building pressure in a sealed tube. No fans/heatsinks needed — slots
  + spacing suffice. **Place vents on outboard/bottom faces** — off the grip
  surfaces (a hand over the vents defeats them) and not aimed at the RF blades.
- Cell/contact dimensions TBD (owner measures); power/BMS stays owner-external.

### Seams between parts (the real design work)
- **Registration:** locating pins / lips / rabbets at mating seams so parts
  self-align instead of being held square by hand while screwing.
- **Through-fastening:** the **two side 18650 holders are the PRIMARY front-to-
  back tie** (Structure 3 — heat-set inserts into both back plates clamp the slab
  as one unit). A few long axial screws / threaded standoffs front-cover →
  screen-plate → back-plate → back-cover are OPTIONAL belt-and-suspenders on top
  of that, not the main structure.
- **Open-for-later:** exact grid pitch; whether a separate thin **top antenna
  rail** is better than an integral back-cover top (lets bulkhead spacing be
  re-drilled without reprinting the cover); where the P4 specifically mounts.

## Material plan: PLA to prototype, PETG for final (NOT ABS)
- **PLA for prototyping** — dimensionally stable (no warp), stiff, easy to print,
  fast iteration. The large flat backplane with blade mounts is the classic
  ABS-warp victim; PLA holds flat so test fits stay meaningful. Stiffness also
  protects solder joints on the mounting plate.
- **PETG for final / field parts** — ~80°C heat tolerance (survives a warm car;
  PLA softens ~50-60°C and can sag on a dashboard), tougher for a carried device,
  far less warp than ABS. Slightly hotter nozzle / slower / can string — printer
  settings only, no design impact.
- **ABS: skip.** Its only edge over PETG is marginal, and its warp is a liability
  for the big flat parts.
- **Material-agnostic design:** same `.scad` + STL print in either — pick
  material at SLICE time. Prototype in PLA, reprint finals in PETG, no redesign.
- Design in vents/airflow regardless (P4 + DC-DC + backlights + RF blades make
  internal heat; PLA especially benefits).

## Tooling / workflow (when we start generating geometry)
- **OpenSCAD** is the path: parametric, text `.scad` source Kiro writes directly;
  owner renders + exports STL + slices. (Alternatives if ever needed: Python
  `CadQuery` / `build123d` / `numpy-stl` / `trimesh` — still code-authored.)
- Build in MODULES (screen plate + front cover, blade back plate + frames + back
  cover, the two side 18650 holders — see the multi-part breakdown) so one
  dimension change updates everything and parts print independently.
- **Everything is driven by measured variables.** Calipers beat datasheets;
  datasheet mechanical dims are often wrong, so the FIRST print of any part is a
  TEST FIT, always.

## Division of labor + known limits (agreed with owner)
- **Kiro is blind to the physical parts** — every hole/cutout is only as correct
  as the dimensions provided. Owner OWNS dimension-gathering; a wrong dimension is
  the owner's to fix, not a reason to distrust the output. True partnership here.
- **Owner does physical validation** — has calipers + tools for rudimentary
  fit/tolerance checking and eyes-on assembly, which closes the iterate loop Kiro
  cannot (shrinkage, clearance, cable bend radius, real fit).
- **Iterative by nature:** measure → parameterize (`.scad`) → render/slice →
  print ONE module/coupon → test fit → adjust variables → repeat. Not a one-shot
  "generate the STL and print it." Print a single mount before a whole plate.

## MEASURED PARTS (recorded dimensions — basis for the eventual `.scad`)
_Source: vendor mechanical drawings (2026-11-01). Datasheet dims, so the FIRST
print of any frame is STILL a test fit per the rule above. Units mm._

### Waveshare ESP32-S3-Touch-LCD-3.5 Rev2.0 (the WORK display, front-left, landscape)
- **Overall front glass / face: 92.44 × 61.00**, corner radius **R6.00** (rounded
  — the bezel must match).
- **Visible/active screen: 73.44 × 48.96**, OFFSET (not centered): inset **9.50**
  from the RIGHT glass edge, **6.02** from the TOP. Aperture must use this offset.
- **Depth profile:** glass **1.10**, PCB+components **7.50**, total **11.50**
  (budget ≥11.5 behind the face, plus FPC/connector clearance).
- **Mounting holes: M2** (NOT M3), four-corner rectangle **72.00 (H) × 48.50 (V)**;
  right-side insets top **6.25**, bottom **10.22**.
- **Left edge:** display FPC ribbon (at ~14.23 / 20.00 up the left) + CHG, PWR,
  Micro-USB, MIC, RTC_BKP, BAT connector cluster.
- **Top edge:** PWR / RESET / BOOT buttons (spacing 11.00 / 8.50 / 8.50), SPK.
- **Right side:** microSD slot + a second FPC connector.
- **Bottom edge:** the GPIO header row.

### CYD (ESP32-2432S028-class — the STATUS display, front-right, portrait)
- **Board size: 86 × 50.**
- **Visible screen area: 59.5 × 45** (front aperture).
- **Mounting holes: Ø3.2 (M3 clearance)**, four-corner; top-right inset ~**4 / 4**.
- **Right edge = the port stack:** P5 (UART) ~15 from top, USB-C ~27, Micro-USB
  ~40.5. Keep this edge reachable (USB-C = charge/flash) — must not be pinned
  against the right battery bookend.
- **Bottom edge:** microSD slot at ~47.5 across, ~11 wide. (NOTE: this rig logs
  SD hub-side, so the CYD's own microSD may be unused — confirm before cutting a
  slot for it.)
- **Other features (not mount-critical):** RST/Boot buttons + Speaker pads top,
  Light Sensor lower-left, RGB LED (~30.5 / 11.5), P3/CN1 GPIO headers bottom.

### Cross-part notes that affect the front face
- **Hole sizes DIFFER: Waveshare M2 vs CYD M3** — per-panel standoff/boss sizes.
- **Port edges are on OPPOSITE sides** (Waveshare ports/FPC LEFT; CYD ports
  RIGHT) and the two panels are rotated differently (W-S landscape, CYD portrait),
  so "left/right board edge" maps to different physical faces. Resolve each
  panel's rotation → which physical face its ports land on when placing them vs.
  the battery bookends. Do NOT assume the front face is mirror-symmetric.
- **Apertures are not symmetric:** W-S has R6 rounded corners + an offset active
  area; CYD is square-ish/centered. Each gets its own correctly-offset cutout.

## Dimensions to gather (remaining parts)
_Fill with caliper measurements when starting. Nothing here is a guess to build from._
- **Per-display glass-face → mounting-hole seating-plane depth** (Waveshare AND
  CYD): governs the coplanar-standoff heights for Structure 1. Measure for both.
- P4 board (P4-Nano for the 8.8" path, or current hub board): outline, holes,
  connector positions, Bus-A/Bus-B header locations.
- Radio blades (**one row per blade — they are NOT uniform**): each board's
  width, height, mount-hole positions, header orientation, and where its antenna
  SMA pigtail originates. This per-blade table drives the back bank + case width.
- GPS: module outline + ceramic patch size/orientation; antenna-connector type.
- 18650 holders (the two bookend side-columns, Structure 3): cell diameter/length
  + the two-cells-per-side seating, contact/terminal plan, and the insert
  positions where the holder meets BOTH back plates. Enough to size the cell
  pockets, inter-cell gap, and vent slots (power/BMS stays owner-external).

## Start point when we build
Begin with the **backplane + blade mounts** — that one module constrains both the
RF spacing and the electrical layout, so it's the right first parametric part.
