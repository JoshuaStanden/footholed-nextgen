#!/usr/bin/env python3
"""
tui_textual.py — FH_Medusa bench console, Textual edition (CROSS-PLATFORM).

This is the Windows-native / recommended TUI. It works in Windows Terminal,
PowerShell, VS Code's terminal, macOS Terminal, and Linux — because Textual
handles cross-platform input/rendering properly (unlike stdlib curses, which is
Unix-really). A single press of c / m opens the command bar / menu (alt combos
don't decode reliably across terminals, so plain keys are the trigger).

    pip install textual
    python3 tui_textual.py --sim                  # drive from the simulator
    python3 tui_textual.py COM5                    # real hub on Windows
    python3 tui_textual.py /dev/tty.usbmodemXXXX   # real hub on mac/linux

It reuses the SAME MedusaModel as the curses tui.py — only the rendering layer
differs. Model layer = UI-agnostic; this is just a second view on it.
"""
import sys

# the curses TUI file holds the shared, UI-agnostic model
from tui import MedusaModel
from medusa import MedusaClient
from medusa.session import Busy

try:
    from textual.app import App, ComposeResult
    from textual.containers import Horizontal, Vertical
    from textual.widgets import Static, DataTable, RichLog, Input
    from textual.binding import Binding
    from rich.text import Text
    _HAVE_TEXTUAL = True
except Exception:
    _HAVE_TEXTUAL = False


if _HAVE_TEXTUAL:

    class MedusaApp(App):
        CSS = """
        #status { height: 1; background: $panel; color: $text; }
        #stream { height: 1fr; min-height: 5; border: round $primary; }
        #body   { height: 2fr; }
        #left   { width: 1fr; border: round $primary; }
        #detail { width: 1fr; border: round $primary; }
        #now    { height: 1; background: $panel; }
        #cmd    { height: 3; display: none; border: round $accent; }
        #cmd.show { display: block; }
        """
        BINDINGS = [
            Binding("c", "command", "Command"),
            Binding("m", "menu", "Menu"),
            # priority: the focused DataTable otherwise swallows End itself, so
            # "jump to newest / re-arm follow" silently did nothing there.
            Binding("end", "newest", "Newest", priority=True),
            Binding("alt+c", "command", "Command", show=False),
            Binding("alt+m", "menu", "Menu", show=False),
            Binding("q", "quit", "Quit"),
        ]

        def __init__(self, model: MedusaModel, use_sim: bool):
            super().__init__()
            self.model = model
            self.use_sim = use_sim

        def compose(self) -> ComposeResult:
            yield Static(" Medusa", id="status")
            yield RichLog(id="stream", highlight=False, markup=True)
            with Horizontal(id="body"):
                yield DataTable(id="left")
                yield Static("(no selection)", id="detail")
            yield Static(" NOW: streaming…   c command · m menu · q quit", id="now")
            yield Input(placeholder="command e.g. nrf24 0 80 · Enter run · Esc close", id="cmd")

        def on_mount(self):
            self.model.m.connect(use_sim=self.use_sim)
            t = self.query_one("#left", DataTable)
            self._cols = t.add_columns("sel", "device / event", "seen")
            t.cursor_type = "row"
            self._shown = {}          # row key -> (star, label, seen) last drawn
            self._cursor_idx = -1     # last cursor index we set
            self._stream_len = 0
            self._rebuilding = False
            self.set_interval(0.25, self._refresh)

        def _refresh(self):
            # status bar — shared model.status_bar() cells, color per state
            # (REMIX redesign). One source of truth with the curses view.
            _CLR = {"ok": "green", "warn": "yellow", "bad": "red bold", "idle": "dim"}
            cells = self.model.status_bar()
            seps_after = {0, 3, 4}
            parts = []
            for i, cell in enumerate(cells):
                safe = cell.text.replace("[", r"\[")
                parts.append(f"[{_CLR.get(cell.state, 'white')}]{safe}[/]")
                if i in seps_after:
                    parts.append("[dim]│[/]")
            self.query_one("#status", Static).update(" " + "  ".join(parts) + " ")
            # stream (append only the new lines)
            with self.model.lock:
                lines = self.model.stream
                new = lines[self._stream_len:]
                self._stream_len = len(lines)
            log = self.query_one("#stream", RichLog)
            # color token -> Rich style (black-bg palette). green_dark/light are
            # true-ish greens; orange via rgb for 256-color terminals.
            _STREAM_STYLE = {
                "blue": "blue", "green_light": "bright_green",
                "green_dark": "green", "yellow": "yellow",
                "white": "white", "orange": "rgb(255,135,0)",
            }
            # BENCH FIX (2026-10-30): data goes in as rich Text, never as a
            # markup string. Markup strings also expand :shortcode: emoji, so a
            # MAC like c3:81:b7:ab:ef:1a rendered ":ab:" as the AB-button emoji.
            for ln, color in new:
                style = _STREAM_STYLE.get(color, "white")
                log.write(Text(ln, style=style))
            # device table. BENCH FIX (2026-10-30): this used to clear() and
            # rebuild every row 4x/s and re-move the cursor each time, which
            # made the table flicker/jump ("twitchy") once real traffic flowed.
            # Now rows are keyed: new rows are appended, existing rows get only
            # the cells that changed, and the cursor moves only when its target
            # changes. Rows are never reordered (model is first-seen order).
            t = self.query_one("#left", DataTable)
            rl = self.model.rows_list()
            self._rebuilding = True          # suppress highlight-handler during updates
            for r in rl:
                cells = ("★" if r.star else " ", r.label, f"x{r.seen}")
                prev = self._shown.get(r.key)
                if prev is None:
                    t.add_row(*(Text(c) for c in cells), key=r.key)   # Text: no emoji/markup
                elif prev != cells:
                    for col, old, new in zip(self._cols, prev, cells):
                        if old != new:
                            t.update_cell(r.key, col, Text(new))
                self._shown[r.key] = cells
            # move the cursor to the identity-selected row (follow->newest, else
            # the sticky key), but only when that target actually changed.
            idx = self.model.sel_index()
            if 0 <= idx < len(rl) and idx != self._cursor_idx:
                try:
                    t.move_cursor(row=idx)
                    self._cursor_idx = idx
                except Exception:
                    pass
            self._rebuilding = False
            # detail from selected
            sel = self.model.selected()
            self.query_one("#detail", Static).update(Text(sel.detail if sel else "(no selection)"))
            # now-line
            foll = " ▶following" if self.model._follow else ""
            self.query_one("#now", Static).update(
                f" NOW: {self.model.now}{foll}   ↑↓ select · end newest · c cmd · m menu · q quit")

        def on_data_table_row_highlighted(self, ev):
            # Only a USER-driven highlight change disengages follow + sets the
            # sticky key; programmatic rebuilds (guarded) must not.
            if getattr(self, "_rebuilding", False):
                return
            # RowHighlighted is delivered AFTER _refresh returns, so the guard
            # above misses it; a highlight landing on the row we just moved to
            # is our own move_cursor, not the user. (Without this, follow mode
            # switched itself off right after startup.)
            if ev.cursor_row == getattr(self, "_cursor_idx", -1):
                return
            # A stale highlight (e.g. the row-0 highlight from the first
            # add_row) no longer matches where the cursor is now: ignore it.
            if ev.cursor_row != ev.data_table.cursor_row:
                return
            rl = self.model.rows_list()
            if 0 <= ev.cursor_row < len(rl):
                self.model._sel_key = rl[ev.cursor_row].key
                self.model._follow = False

        def action_newest(self):
            self.model.jump_newest()

        def action_command(self):
            cmd = self.query_one("#cmd", Input)
            cmd.add_class("show"); cmd.focus()

        def action_menu(self):
            # minimal menu: cycle a hint in the now-line (full menu screen is a
            # follow-up; the command bar already covers firing verbs)
            self.model.now = "menu: (WiFi·BLE·nRF24·NFC·SubGHz·LoRa·GPS·Tools) — type via c"

        def on_input_submitted(self, ev):
            line = ev.value.strip()
            self.query_one("#cmd", Input).value = ""
            self.query_one("#cmd", Input).remove_class("show")
            self.set_focus(None)
            if line:
                self._run_command(line)

        def _run_command(self, line):
            # Parsing/dispatch lives in the shared model (verb grammar, human
            # units, reserved verbs) — both TUIs behave identically.
            if line.strip():
                self.model.now = f"running {line.split()[0].lower()}…"
                self.model.run_command_async(line)   # off-thread: UI stays live

        def on_unmount(self):
            self.model.m.close()


def main():
    args = sys.argv[1:]
    if "--selftest-model" in args:
        from tui import _selftest_model
        _selftest_model(); return
    if not _HAVE_TEXTUAL:
        print("Textual not installed. Run:  pip install textual\n"
              "(Or use the stdlib fallback:  python3 tui.py --sim)")
        sys.exit(1)
    use_sim = "--sim" in args
    args = [a for a in args if a != "--sim"]
    if not use_sim and not args:
        print(__doc__); sys.exit(1)
    port = args[0] if args else "sim"
    MedusaApp(MedusaModel(MedusaClient(port)), use_sim).run()


if __name__ == "__main__":
    main()
