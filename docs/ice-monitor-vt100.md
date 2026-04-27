# `ice monitor` — vt100 inner-terminal emulator

## Goal

Replace the line-oriented `tui_log` byte buffer with a real vt100
emulator so `ice monitor` correctly displays interactive chip-side
programs that drive the terminal with cursor moves, line clears, and
DSR queries — primarily ESP-IDF's linenoise-based console REPL
(`examples/system/console/{basic,advanced}`), but the same machinery
covers any future curses-ish on-chip program.

The chip's serial stream is *always* a vt100 stream.  Today we get
away with treating it as line-oriented because most chip output is
plain `printf` with `\n`.  This plan stops getting away with it.

## Why this is needed

Linenoise (`esp-idf/components/console/linenoise/linenoise.c`) drives
the terminal with byte-level CSI sequences and *no trailing `\n`*:

| Operation                | Sequence                       |
| ------------------------ | ------------------------------ |
| Erase to EOL             | `\x1b[0K`                      |
| Cursor right N           | `\x1b[<N>C`                    |
| Cursor left N            | `\x1b[<N>D`                    |
| Cursor up/down N         | `\x1b[<N>A` / `\x1b[<N>B`      |
| Carriage return          | `\r`                           |
| Clear screen (Ctrl-L)    | `\x1b[H\x1b[2J`                |
| Query cursor position    | `\x1b[6n` → expects reply      |
| Probe device status      | `\x1b[5n` → expects reply      |
| Probe right margin       | `\x1b[999C` then `\x1b[6n`     |

The current monitor (`cmd/target/monitor/monitor.c:639`,
`tui.c:1021` `tui_log_append`) splits only on `\n`, so linenoise's
in-place redrawn prompt accumulates in `pending` forever.  CSI bytes
pass through verbatim into a TUI frame that owns absolute cursor
positioning, so cursor moves collide with the TUI's own
`term_move` / clear-to-EOL.  DSR replies travel out to the user's
terminal and the user-terminal's reply gets fed back to the chip as
keyboard input — pollution in both directions.

Result: typing into the console example produces a smear of prompts,
SGR fragments, and runaway lines instead of an editable input line.

## Architecture

Three layers, with one new file (`vt100.c`) sitting between the
existing `tui.c` and `term.c`.

```
chip side                              user side
─────────                              ─────────
serial.c    (byte I/O)                 term.c    (byte+escape I/O,
   │                                              platform-varying)
   ▼                                       ▲
vt100.c     (protocol parser → grid)        │
   │  pure C, no I/O                        │
   │  + reply sbuf the caller drains        │
   ▼                                        │
tui.c       (composer)                      │
   live grid (from vt100) +                 │
   scrollback ring +                        │
   status bar + modals  ──────────► term_*  │
```

Locked-in design decisions (already discussed; do not relitigate
without a strong reason):

- **`vt100.c` is platform-free pure C.**  No `serial_*`, no `term_*`,
  no syscalls, no `#ifdef`.  Like `yaml.c` and `sbuf.c`, it sits at
  the project root and is callable in isolation.  All I/O happens in
  the caller (the monitor's serial-read loop).
- **Reply delivery: internal sbuf, drained by the caller.**  The
  vt100 owns a `struct sbuf reply` that DSR-style dispatches append
  to.  After every `vt100_input(...)` the caller checks
  `vt100_reply(V)`, writes any pending bytes back over `serial_write`,
  and `sbuf_reset`s the buffer.  Rationale: keeps `vt100.c` pure;
  matches existing root-helper conventions; the call site is exactly
  one place in `cmd/target/monitor/monitor.c` so "remember to drain"
  is not a real risk.
- **Inner grid size matches the outer terminal at session start.**
  Outer cols × (rows minus status-bar overhead).  When the outer
  resizes, the inner grid resizes too; the chip learns the new size
  on its next `getColumns()` probe (linenoise re-probes per prompt,
  so the next prompt sees the new size).
- **`tui.c` owns the scrollback.**  `vt100.c` exposes only the live
  grid (rows × cols of cells).  When a row scrolls off the top of
  the inner grid (via implicit scroll on cursor-past-bottom, or via
  SU within a scroll region), `vt100.c` notifies `tui.c` (via a
  callback or a "scrolled-off lines" accessor); `tui.c` materialises
  that row into a scrollback line.  Search and the freeze-on-modal
  pattern operate over (scrollback + frozen grid snapshot).
- **Outer-terminal capability ceiling is handled by `term.c`, not by
  `vt100.c` or the renderer.**  `term.c` already abstracts VT-vs-Win32
  Console API (see `term.h:30-33` and `term.h:285-289`,
  `platform/win/io.c console_write_legacy`).  The renderer emits VT
  shapes; `term.c` downgrades on legacy hosts.  We don't add a
  separate Windows render path.

Two-ceilings principle (write it down so the architecture stays
honest):

- **Inner modeling correctness** is bounded by what the chip emits.
  `vt100.c` must track wide chars, deferred wrap, scroll regions,
  etc. correctly *regardless of what the outer terminal can render*,
  because the chip's edit math depends on it.
- **Outer rendering capability** is bounded by what the user's
  terminal can do.  Truecolor, italics, wide CJK on legacy console,
  etc. degrade in the renderer at paint time.

These are separate concerns and live in different files.

## Data journey (worked example)

Chip emits `\x1b[32mHi\n` (7 bytes — set green, write "Hi", newline).
Cursor starts at (4, 0); grid is 80×24.

1.  `serial_read()` returns 7 bytes into `buf`.
2.  Monitor calls `vt100_input(&V, buf, 7)`.
3.  Parser state machine:
    - `\x1b` → ESCAPE
    - `[` → CSI_ENTRY
    - `3`, `2` → CSI_PARAM, params = `[32]`
    - `m` → final byte; dispatch SGR(32); `V.sgr.fg = green`
    - `H` → ground; putc: `grid[4][0] = {'H', sgr=green}`, cursor → (4,1)
    - `i` → putc: `grid[4][1] = {'i', sgr=green}`, cursor → (4,2)
    - `\n` → cursor → (5, 2); no scroll (row 5 < bottom_margin)
4.  `vt100_reply(&V)->len == 0` → no `serial_write`.
5.  Monitor sets `dirty = 1`.
6.  Render tick: `tui.c` composes a frame from (scrollback, live grid,
    status bar).  For each dirty row, emit `term_move(row, 1)`,
    `term_sgr("32")`, `term_putc('H')`, `term_putc('i')`,
    `term_clear_to_eol()`, `term_sgr_reset()`.
7.  `term.c` writes the bytes (VT on POSIX/modern Windows, Console
    API on legacy Windows).
8.  Outer terminal renders.

Reply path differs.  Suppose the chip sends `\x1b[6n` instead:

1.  `vt100_input(&V, "\x1b[6n", 4)`.
2.  Parser dispatches DSR(6); appends `\x1b[<row>;<col>R` to
    `V.reply` based on the inner cursor.
3.  Monitor: `r = vt100_reply(&V); serial_write(s, r->buf, r->len);
    sbuf_reset(r);`.
4.  No grid mutation, `dirty = 0`, no redraw.

The reply travels chip → vt100 → chip.  The outer terminal never sees
it.  That is the architectural payoff: linenoise's terminal probing
is fully self-contained inside ice.

## Module surface

New files at the project root:

- `vt100.h` — public API (struct definitions, function prototypes).
- `vt100.c` — implementation.
- `t/test_vt100.c` + `t/<NNNN>-vt100.t` — TAP unit tests.

`vt100.h` API sketch (subject to refinement during phase 1):

```c
struct vt100;          /* opaque */

/* SGR bits packed into a single uint16 or uint32 -- bold, underline,
 * italic, reverse, plus 8/16/256/truecolor fg/bg.  Concrete layout
 * decided in phase 2 once the cell struct is settled. */
struct vt100_sgr;

struct vt100_cell {
    uint32_t cp;       /* Unicode codepoint; 0 = blank */
    struct vt100_sgr sgr;
    /* width=2 spacers represented as cp=0 with a flag bit --
     * decided in phase 2 if/when wide-char support lands. */
};

/* Lifecycle. */
struct vt100 *vt100_new(int rows, int cols);
void vt100_free(struct vt100 *V);
void vt100_resize(struct vt100 *V, int rows, int cols);

/* Input: feed chip bytes.  Idempotent and re-entrant-safe (no I/O,
 * no global state).  Replies, if any, accumulate in the reply sbuf. */
void vt100_input(struct vt100 *V, const void *buf, size_t n);

/* Reply drain: caller writes r->buf[0..r->len] to serial then
 * sbuf_reset(r).  Returns a pointer to the internal sbuf -- never
 * NULL.  Length is 0 when there is nothing to send. */
struct sbuf *vt100_reply(struct vt100 *V);

/* Grid accessors used by the renderer. */
int vt100_rows(const struct vt100 *V);
int vt100_cols(const struct vt100 *V);
const struct vt100_cell *vt100_cell(const struct vt100 *V, int row, int col);
void vt100_cursor(const struct vt100 *V, int *row, int *col, int *visible);

/* Scrolled-off-top notification.  Called by tui.c each render tick.
 * vt100 internally queues rows that scrolled off the top of the grid
 * since the last drain; the caller materialises them into scrollback
 * and then calls vt100_drain_scrolled(V). */
int vt100_scrolled_count(const struct vt100 *V);
const struct vt100_cell *vt100_scrolled_row(const struct vt100 *V, int i, int *cols);
void vt100_drain_scrolled(struct vt100 *V);
```

Every entry point is pure-C data-in/data-out.  No callbacks, no I/O.

## Phases

The breaking switchover (`tui_log_append` → `vt100_input`) goes
**last**.  Through phases 1-6 the live monitor still uses the
existing line-oriented path; only the unit tests exercise the new
emulator.  At phase 7 we flip the dispatch hook in one commit.

### Phase 1 — vt100.c skeleton + parser state machine

- Create `vt100.h`, `vt100.c` with lifecycle (`vt100_new`,
  `vt100_free`, `vt100_resize`) and a Paul-Williams CSI parser
  state machine.  Parser eats bytes; no dispatch wired yet (every
  final byte goes to a stub that updates a counter for tests).
- Cell grid allocated as `rows × cols` flat array; cursor + SGR
  state stubbed.
- `t/test_vt100.c` covers: parser splits CSI / OSC / charset /
  printable bytes correctly across `vt100_input` boundaries
  (a CSI sequence split mid-byte across two calls must still
  dispatch once).
- `t/<NNNN>-vt100.t` shell wrapper following `t/0001-sbuf.t`.

**Acceptance**: parser correctly classifies every byte of a
captured linenoise trace into (printable / C0 / CSI / OSC / charset)
with no leftover state at end of trace.

### Phase 2 — Putc + cursor + clears + SGR

- `putc()` with **deferred wrap**: writing to the rightmost column
  sets `pending_wrap`; the wrap fires on the next printable byte.
  Any explicit cursor-positioning CSI clears the flag.  `\r` clears
  the flag.
- CSI dispatch: `CUU`/`CUD`/`CUF`/`CUB`, `CUP`/`HVP`, `CHA`, `EL`
  (params 0/1/2), `ED` (params 0/1/2), `DECSC`/`DECRC` (`ESC 7` /
  `ESC 8`), `SGR` (track into cell sgr field), modes `?7` (autowrap
  on/off) and `?25` (cursor visible).
- Per-op tests with byte input + grid assertion.  Specifically test
  the deferred-wrap edge case: write 80 chars to a 80-wide grid,
  then `\r\x1b[0K` clears the *same* row, not the next.

**Acceptance**: linenoise's `refreshSingleLine` (linenoise.c:567-603
 in esp-idf) reproduced byte-for-byte against the vt100 produces
the expected grid state at every step.

### Phase 3 — Scroll regions, line/char ops

- `DECSTBM` (set top/bottom margins).
- `IL`/`DL` (insert/delete lines within scroll region).
- `ICH`/`DCH` (insert/delete characters).
- `SU`/`SD` (scroll up/down).
- Implicit scroll on cursor-past-bottom-margin: top row of region
  enqueued into the scrolled-off list, lines shift up, last row
  blanks.
- Skip but consume: charset escapes (`ESC ( B`), OSC sequences
  (everything between `ESC ]` and `BEL`/`ST`), alt-screen
  (`?1049`) — track the mode flag but don't double-buffer the grid;
  defer dual-buffer support to a follow-up if/when a chip program
  needs it.

**Acceptance**: a 100-line stream of `printf("hello %d\n", i)`
produces the expected (scrollback-of-76 + visible-24) split when the
inner grid is 24 rows tall.

### Phase 4 — DSR synth + reply drain

- `\x1b[5n` → `\x1b[0n` appended to reply sbuf.
- `\x1b[6n` → `\x1b[<row>;<col>R` (1-based) appended to reply sbuf.
- Reply cap: bound the reply sbuf at some sane ceiling (e.g. 256 B);
  if a single `vt100_input` would overflow, drop the oldest reply
  bytes (linenoise re-probes; no crash).
- Test: feed `\x1b[999C\x1b[6n` against an 80×24 grid; assert
  `vt100_reply(V)->buf == "\x1b[1;80R"` (cursor clamps at right edge,
  reply reflects clamped column).

**Acceptance**: a recorded linenoise startup probe (`getColumns`)
fed into the vt100 produces the exact reply byte sequence linenoise
expects.  Capture this as a golden fixture.

### Phase 5 — Scrollback bridge

- `tui.c` keeps the existing `tui_log` ring but renames the field
  semantics: lines in the ring now come from `vt100_drain_scrolled`,
  not from `\n`-splitting raw bytes.  `pending` goes away — the
  in-progress edit lives in the live grid.
- Render tick: pull `vt100_scrolled_count`; for each, materialise a
  scrollback line (cells → coloured byte stream) and push it into
  the ring; then `vt100_drain_scrolled`.
- `tui_log_freeze` semantics extend to "snapshot scrollback ceiling
  + snapshot the live grid as a frozen 2D array."  Modal overlays
  paint over the snapshot.  `tui_log_unfreeze` discards the
  snapshot; live grid resumes.
- Search rewires across (scrollback ring + frozen-grid-snapshot or
  live-grid).  Live-grid search re-scans on grid mutation (cheap:
  rows × cols ≤ a few thousand cells).
- Resize: outer resize → `vt100_resize` → grid reallocated with
  reflow rules (TBD; simplest is "preserve cursor relative to new
  size, blank-fill the rest").  Chip re-probes at next prompt.

**Acceptance**: with `vt100.c` still off the wire, the renderer
exercised via a synthetic test that drives a vt100 instance directly
produces frame output bit-identical to the line-oriented version
for plain `printf("...\n")` traffic.

### Phase 6 — Renderer rewrite

- `tui.c`'s `render_log_line` becomes `render_grid_row` (plus the
  existing scrollback-row path).  Cells emit grouped runs of equal
  SGR with a single `term_sgr` per run.
- Modal overlays (search prompt, help info) unchanged; they paint
  over the composed frame as before.
- Cursor visibility: when not in a modal and the grid says cursor is
  visible, paint cursor at the live grid's cursor cell at frame end
  (terminal moves there).
- Status bar / footer composition unchanged.

**Acceptance**: `make test` passes; manual smoke test against a
`hello_world` sketch on a real chip shows identical output to today
(no visible regression on the line-oriented case).

### Phase 7 — Switchover (last)

- `cmd/target/monitor/monitor.c:639` changes:

  ```c
  /* before */
  if (n > 0) {
      tui_log_append(&L, buf, (size_t)n);
      dirty = 1;
  }

  /* after */
  if (n > 0) {
      vt100_input(&V, buf, (size_t)n);
      struct sbuf *r = vt100_reply(&V);
      if (r->len) {
          serial_write(s, r->buf, r->len);
          sbuf_reset(r);
      }
      dirty = 1;
  }
  ```

- The vt100 instance `V` is initialised at monitor startup with the
  outer terminal size (less status-bar rows).  On `term_resize_pending`
  the existing `tui_log_resize` call adds a `vt100_resize` call with
  the new inner-grid dimensions.
- Drop `tui_log_append`'s `\n`-splitting + `pending` code.  The
  signature stays (the renderer's scrollback path still calls into
  the ring), but bytes no longer enter via this path.
- Manual test: `examples/system/console/basic` flashed onto a chip;
  type into the prompt, exercise backspace, history (up/down arrow),
  Tab autocomplete, Ctrl-L clear-screen, multi-line edit with a long
  command.  All should behave exactly like a native terminal.

**Acceptance**: the console example is fully usable through
`ice monitor`.  `make test` passes.  No regression in
`hello_world`-style log streaming.

## Non-goals (deferred)

The plan deliberately defers anything that the ESP-IDF console
example doesn't need:

- **Wide chars (CJK, emoji)** — chip overwhelmingly emits ASCII +
  SGR.  When a use case appears, add `wcwidth`-style tables and
  spacer cells in `vt100.c`; the renderer adapts.
- **Truecolor / 256-color** — linenoise uses basic SGR; logging
  uses 16-color.  Cell SGR fits in `uint16_t` with 16-color fg+bg.
- **Alt screen double-buffering** — the mode flag is tracked but
  the grid isn't duplicated.  If `top` or `vim` ever runs on the
  chip we revisit.
- **Sixel / image protocols** — out of scope.
- **Mouse reporting** — out of scope.
- **Bidi / RTL** — out of scope.
- **Programmable function keys** — out of scope.

## Open questions

These do not block phase 1 but should be settled before they bite:

1.  **Resize reflow policy.**  When the outer terminal resizes, what
    happens to the inner grid contents?  Three options: (a) blank a
    fresh grid and let the chip redraw on next prompt; (b) preserve
    contents anchored at top-left, blank the new region; (c) full
    reflow (re-wrap long lines).  Option (a) is simplest and matches
    what most terminal emulators do on resize when nothing requests
    refresh.  Lean toward (a) unless a real-world workflow shows
    flicker.

2.  **Scrolled-off API: callback or pull?**  Two shapes:
    - Pull: `vt100_scrolled_count` + `vt100_scrolled_row(i)` +
      `vt100_drain_scrolled` (sketched above).
    - Callback: `vt100_set_scroll_cb(V, cb, ctx)` invoked synchronously
      from inside `vt100_input`.
    Pull keeps `vt100.c` purer (no callbacks at all) and matches the
    reply-drain pattern.  Default to pull; revisit if a use case
    needs synchronous scroll handling.

3.  **Cell struct memory budget.**  An 80×24 grid is 1920 cells.  At
    `{cp: u32, sgr: u16, flags: u16}` = 8 bytes per cell that's
    15 KB; at u32+u32 = 8 KB.  Either is comfortable.  Lock in
    layout in phase 2 once we know the SGR bit field.

4.  **Status-bar overhead in inner-grid sizing.**  The current TUI
    reserves row 1 for status and row N for footer (`tui.h:300-308`).
    Should the inner grid's `rows` exclude both?  Yes, simplest.
    Status/footer remain under `tui.c`'s control; they're not part
    of the chip's terminal model.

## References

- Existing monitor: `cmd/target/monitor/monitor.c:88-866`
  (porcelain at `cmd/monitor/monitor.c:88-136` delegates here).
- Existing log buffer: `tui.h:310-355` (`struct tui_log`),
  `tui.c:1021` (`tui_log_append`).
- Existing CSI skip-for-width code (to be replaced by real parser):
  `tui.c:1048-1071` (`log_skip_csi`), `tui.c:1079` (`log_visual_rows`).
- Existing render path: `tui.c:1158-1300` (`render_log_line`).
- Modal freeze pattern: see `tui_log_freeze` /
  `tui_log_unfreeze` (`tui.h:374-392`); we mirror the contract for
  the grid snapshot.
- Outer-terminal abstraction: `term.h:30-33` (legacy-Windows
  fallback), `term.h:283-293` (raw-mode output helpers),
  `platform/win/io.c console_write_legacy` (VT-to-Console-API
  parser — already exists, no work needed there).
- Existing root-helper conventions: `sbuf.h` (value-type-with-INIT
  pattern, used for the reply buffer), `yaml.h` (opaque-heap-parse
  pattern, closer match for `struct vt100`).
- Project conventions: `CONTRIBUTING.md` §"Repository layout",
  §"Platform abstraction", §"`cmd/` layout".
- Linenoise upstream (read-only reference):
  `~/work/esp-idf/components/console/linenoise/linenoise.c`.
- ESP-IDF console example to drive manual testing:
  `~/work/esp-idf/examples/system/console/basic`.
