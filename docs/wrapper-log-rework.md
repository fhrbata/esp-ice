# Wrapper / progress / log rework

## Problem

`process_run_progress()` writes a per-call log file under
`<build>/.ice/logs/` (or `~/.ice/logs/` outside a project) and
maintains a `last` pointer file that `ice log` reads to surface the
most recent run. Two structural issues surface in the wrapper +
plumbing pattern used by commands like `ice flash`:

1. **Nested-call clobber.** When a porcelain wrapper (`cmd_flash`)
   spawns hidden plumbing (`cmd___flash`) via `process_run_progress`,
   the child's own `process_run_progress` calls — typically from
   `setup_project(PROJECT_BUILT)` triggering `project_build()` —
   write *another* log file AND update `last`. The child's update
   happens after the parent's, so `last` points at the inner build
   log when the run ends. Original user report: `ice flash` failed at
   port scan → `ice log` showed the (successful) build log instead of
   the flash log.

2. **Same-second filename collision.** Filenames are
   `<YYYYMMDD-HHMMSS>-<slug>.log`. Two `process_run_progress` calls
   with the same slug in the same second resolve to the same path;
   `fopen(path, "wb")` truncates, so the second writer silently
   destroys the first writer's log. Easy to hit with the wrapper
   pattern (parent's project_build + child's project_build both run
   inside one wall-clock second on a no-op rebuild).

A targeted fix landed earlier (`cmd___flash_desc.needs =
PROJECT_CONFIGURED`) to stop the inner build entirely. This rework
addresses the underlying design instead of patching one call site.

## Design

### Log identity

- **Drop the `last` pointer mechanism entirely.** No
  `<log-dir>/last` file, no `update_last_pointer()`. The "newest by
  filename timestamp" view that `ice log -n 1` already uses becomes
  the sole answer to "which log is most recent."
- **PID in filenames.** New format:
  `<YYYYMMDD-HHMMSS>-<pid>-<slug>.log`. Uniqueness across nested
  *and* concurrent invocations; no truncation collisions.
- **`ice log` argument shape.**
  - `ice log` (no args) → newest log file by filename timestamp.
  - `ice log <name>` → glob match `<log-dir>/*<name>*.log`. Errors
    if zero or >1 matches; otherwise displays the match.
  - `ice log -n N` and `ice log --all` keep their current behavior.
- **`process_run_progress` hint.** Both success and failure paths
  print the absolute log path *and* a typing-friendly form
  (`run @b{ice log <slug>-<pid>}`) so the user has an unambiguous
  reference when multiple sessions are running.

### Wrapper / plumbing relationship

- Porcelain wrappers (`cmd_flash`) keep invoking their plumbing
  (`cmd___flash`) via `process_run_progress` so a single spinner
  covers the user-visible operation.
- **Plumbing carries `.needs = PROJECT_BUILT`** — the build runs
  *inside* the child. **Wrapper has no `.needs`** and does no
  `setup_project` of its own.
- **Hidden global flag `--ice-wrapped`** distinguishes a child
  running under a wrapper from a top-level invocation. The wrapper
  pushes it into the spawned argv between exe and the subcommand:
  ```
  ice --ice-wrapped __flash --port ... --baud ...
  ```
  When `global_wrapped` is set in the child:
  - `process_run_progress` calls skip log file creation entirely.
  - The read loop always mirrors captured bytes to stdout (implicit
    verbose) so the parent captures everything from the pipe.
  - The on-failure log dump is suppressed; the parent will dump its
    own log when its `process_run_progress` returns non-zero.
  - Phase summary lines (`✓ Building done. (X.Xs)`) still print to
    stdout so they appear in the parent's captured stream as natural
    phase markers.
- Result: **one log file per top-level user invocation**, written by
  the parent. For `ice flash`, the single `<ts>-<pid>-flash.log`
  contains build cmake output, build summary line, port scan output,
  and flash protocol output in the order they happened.

### UX consequence

- `ice flash` displays a single combined `Flashing... (Xs)` spinner
  spanning build + flash. Distinct phase spinners are gone.
- In exchange:
  - No inner-build leakage between log files.
  - No verbose-mode duplication (cmake bytes appear in exactly one
    file).
  - `ice log` always points at the right file (no `last` race).
- On failure, the parent's `dump_failure_log` prints the captured
  stream including the child's own `✓ Building done.` /
  `✗ Building failed.` markers, so the user can identify which
  phase failed without opening the log file.

## Implementation order

Four small steps; each compiles and runs on its own.

### Step 1 — wrapper / plumbing `.needs` + `--ice-wrapped` flag

Files: `cmd/flash/flash.c`, `options.c` (or the global-flags site —
check for `--ice-complete` parsing as the template), `progress.c`,
`ice.h`.

- Revert the prior targeted fix: restore
  `cmd___flash_desc.needs = PROJECT_BUILT` and remove the explanatory
  comment block added with it.
- Drop `cmd_flash_desc.needs` (defaults to `PROJECT_NONE`).
- Add a global `int global_wrapped`; parse `--ice-wrapped` as a
  hidden global flag (mirror the `--ice-complete` plumbing). Not
  shown in `--help`.
- `cmd_flash`: insert `--ice-wrapped` into the spawned argv between
  the exe path and `__flash`.
- `process_run_progress`: branch on `global_wrapped`:
  - Skip `fopen` of the log file.
  - Force `verbose = 1` for the read-and-mirror loop.
  - Skip the on-failure `dump_failure_log` call.
  - Skip the path/hint print at the end (parent owns the user-facing
    output).
  - Phase summary `printf` (`✓ Building done.`) stays — it ends up
    in the parent's captured stream.

### Step 2 — PID-suffix filenames + drop `last`

Files: `progress.c`.

- `build_log_path`: extend the format string to include `getpid()`:
  `%04d%02d%02d-%02d%02d%02d-%d-%s.log`.
- Delete `update_last_pointer()` and every call site.
- Search for any other reference to the `last` file (docs, manuals,
  shell completion notes) and remove.

### Step 3 — `ice log` rework

Files: `cmd/log/log.c`.

- Remove `show_last()` entirely.
- Default (no args) reuses the existing `-n 1` codepath (newest by
  filename — already chronological because of the timestamp prefix).
- Add a positional argument `<name>`:
  - Glob `<log-dir>/*<name>*.log` (or substring match by walking
    the dir + `strstr`).
  - Zero matches → die with a clear error.
  - Multiple matches → die with the list, suggest a more specific
    name.
  - Single match → display.
- Update the manual block (`log_manual.description`,
  `.examples`, etc.) — drop references to `last`, document the
  positional form.

### Step 4 — hint formatting

File: `progress.c`.

- In the non-wrapped branch, update both success and failure hint
  prints:
  - Always include the absolute log path.
  - Always include the short form: `run @b{ice log <slug>-<pid>}`.
- Suppressed when `global_wrapped` is set (already covered by
  Step 1).

## Out of scope (future work)

- **Runtime toggle key** for switching between spinner and live-tail
  rendering during a running `process_run_progress`. Discussed: use
  `term_raw_enter(TERM_RAW_KEEP_SIG)` (with a new `flags` parameter)
  to put the tty in cbreak mode, poll stdin alongside the child
  pipe, flip a `show_live` flag on Ctrl-O. Existing
  `atexit(restore_on_exit)` in `platform/posix/term.c` (line 93,
  164) and `platform/win/term.c` (line 150, 256) handles cleanup on
  `die()`. Independent of this rework — pursue as a separate change.

- **In-process tee** (dup2 stdout/stderr through a pipe to capture
  in-process `printf` output for a session abstraction). Considered
  and dropped — `--ice-wrapped` + the existing subprocess pipe-tee
  in `process_run_progress` solves the concrete problems with much
  less code. Revisit only if the wrapper boilerplate (porcelain
  function whose only job is to spawn plumbing) becomes burdensome
  across many commands.

- **Cross-shell session disambiguation** for `ice log` when two
  terminals are running concurrent ice commands in the same project.
  Not solved by this rework. Pragmatic mitigation: the explicit
  `<slug>-<pid>` form printed by every `process_run_progress`
  invocation gives the user an unambiguous lookup. Further options
  (`getppid`-based filtering, per-tty pointer file) only worth
  building if cross-session confusion becomes a real complaint.
