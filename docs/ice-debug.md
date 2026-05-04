# Real-target debug — `ice debug` + monitor gdb-stub auto-attach

Active plan.  Status: design agreed, not yet implemented.

Two orthogonal features bundled in one plan because they share
context (firmware-side gdb-stub, esp-toolchain gdbs, project
state).  They ship in separate phases — phase 1 stands on its
own, phase 2 builds on the byte-stream filter pattern that's
likely to land between them.

| Feature                    | Trigger        | Transport                  | Firmware req         | Cmd home      | Phase |
|----------------------------|----------------|----------------------------|----------------------|---------------|-------|
| OpenOCD debug              | deliberate     | JTAG → tcp::3333           | none                 | `ice debug`   | 1     |
| Panic-attach gdb           | reactive       | UART, gdb-remote in-band   | `CONFIG_ESP_GDBSTUB` | `ice monitor` | 2     |

---

# Phase 1 — `ice debug` (OpenOCD)

## Goal

Mirror the dual-pane shape of `ice qemu --debug` for real hardware:
gdb in one pane, the chip's UART in the other, per-pane inspect
mode, focus toggle, mouse-aware routing — all the TUI bits already
shipped in `feat/tui-pane-rework` (PR #41).

OpenOCD is the debug host (JTAG / built-in USB-JTAG → gdb stub on
`tcp::3333`).  Defaults come from `project_description.json` so
`ice debug` Just Works on a built project; the plumbing form takes
explicit args and works against any ELF.

## Cmd shape (porcelain / plumbing split)

Mirrors `ice monitor` ↔ `ice target monitor`.

### Plumbing — `cmd/target/debug/debug.c`

Owns the dual-pane orchestrator.  No `.needs` — works against
explicit args, debuggable without a project.

Options:

- `-p`, `--port <dev>` — UART for the chip (auto-detected via
  `esf_find_esp_port()` if omitted).
- `--elf <path>` — symbol file for gdb.
- `--chip <name>` — picks the gdb binary (`xtensa-esp32-elf-gdb` /
  `riscv32-esp-elf-gdb`).  Auto-detected from the port if omitted.
- `--openocd-bin <path>` — explicit binary, else `resolve_tool` +
  auto-install.
- `--openocd-cmd "<args>"` — OpenOCD command-line tail (e.g.
  `-f board/esp32-wrover-kit-3.3v.cfg`).  Required when no project
  is feeding it.
- `--gdb-bin <path>` — explicit binary, else chip-resolved.
- `--gdb-port <int>` — TCP port for the OpenOCD gdb stub
  (default 3333).
- `-b`, `--baud <rate>` — UART baud (default 115200).
- `--scrollback <lines>` — per-pane.

### Porcelain — `cmd/debug/debug.c`

Declares `int cmd_target_debug(int argc, const char **argv);`
extern, fills argv from project state, delegates.

`.needs = PROJECT_BUILT` so `project_description.json` is guaranteed
to exist.  Reads:

- `_project.elf`            (already extracted by `config.c:843`)
- `_project.chip`           (already exposed)
- `_project.openocd-args`   (NEW; from `debug_arguments_openocd`)
- `_project.gdb-prefix`     (NEW; from `monitor_toolprefix`)
- `serial.port` / `serial.baud` (existing; same keys `ice monitor`
  consumes)

## Implementation steps

Small, independently-mergeable.  Each builds and tests on its own.

### 1.1  `project_description.json` extract — `~30 LOC` in `config.c`

Extend the existing block at `config.c:843-857` (which pulls
`project_name` and derives `_project.{mapfile,elf}`) to also
pull `debug_arguments_openocd` → `_project.openocd-args` and
`monitor_toolprefix` → `_project.gdb-prefix`.  No cmd changes
yet — verify with `ice config get _project.openocd-args` on a
built project.

### 1.2  OpenOCD binary resolution — `~30 LOC`

Mirror `cmd/qemu/qemu.c:1567-1622` literally.  Package name
`openocd-esp32`, subpath `openocd-esp32/bin/openocd`.  On miss,
run `ice tools install --tool openocd-esp32 <idf>/tools/tools.json`
via `process_run_progress`.  No cmd to expose this yet — wire it
into the debug cmd in step 1.4.

### 1.3  OpenOCD launcher helper — `~150 LOC`

New helper (probably `cmd/target/debug/openocd.{c,h}` or inline
under `cmd/target/debug/debug.c` for v1 — see open item below).
Spawns the daemon with stderr redirected to a pipe we hold;
tail-poll for `Listening on port \d+ for gdb connections` (esp-idf
does 5 × 500ms; we can match that), or until exit / timeout.
Returns the pid + log fd to the cmd; cmd is responsible for
SIGTERMing the daemon on exit.

Process-group / pdeathsig handling: let the cmd put openocd in
its own process group so `kill(-pgid, SIGTERM)` cleans up reliably
even if OpenOCD has spawned helpers.  `platform/posix/process.c`
already exposes `process_start`; check whether pgid setup is
there or needs adding.

### 1.4  Plumbing cmd — `~250 LOC`

Fork `run_debug` from `cmd/qemu/qemu.c:983-1395`.  The dual-pane
scaffold (rect layout, dpane structs, focus, key routing, status
bar, modals) is reused verbatim.  Differences:

- **Two transports**.  qemu-side: UART comes from a pipe
  (`proc->out`), gdb pane talks to `tcp::3333` via gdb's own
  socket.  Real-hardware-side: UART comes from a `serial *` (open
  the device, set baud, read with `serial_read`); gdb still talks
  `tcp::3333` (OpenOCD), launched the same way.
- **No QEMU process to spawn** — instead, spawn OpenOCD (step 1.3),
  then spawn gdb pty pre-loaded with `target remote :<gdb-port>`
  and the ELF (existing pattern from qemu --debug).
- **Reset hotkey**.  `Ctrl-T+r` already routes through gdb's pty
  in qemu --debug.  Replace the literal `"monitor system_reset"`
  with `"monitor reset halt"` (OpenOCD's command) — one-line diff
  shared via a per-cmd string.
- **No flash image build** — debug doesn't program the chip,
  assumes the firmware is already running.  (Optional `--flash`
  flag could chain `ice flash` first; defer.)

### 1.5  Porcelain cmd — `~80 LOC`

`cmd/debug/debug.c`.  Reads the project keys listed above, builds
argv, calls `cmd_target_debug`.  Manual entry mirrors `ice monitor`'s
porcelain manual.

### 1.6  Tests + manual verification

- Build with `make` and `make CC=/usr/bin/clang clang-tidy`.
- Manual smoke on at least ESP32 (xtensa, separate JTAG adapter)
  and ESP32-C3 (riscv, built-in USB-JTAG).
- Verify the auto-install path: rename `~/.ice/tools/openocd-esp32`
  out of the way, run `ice debug`, confirm install + retry.
- Verify the porcelain reads `debug_arguments_openocd` correctly
  by deleting it from the JSON and watching `ice debug` fail
  helpfully.

## Reusable from existing code

- `cmd/qemu/qemu.c:run_debug` — dual-pane orchestrator.
- `cmd/qemu/qemu.c:1567-1622` — `resolve_tool` + auto-install
  pattern.
- `cmd/qemu/qemu.c:debug_layout`, `dpane`, focus model,
  `Ctrl-T+r` reset hotkey scaffold.
- `cmd/target/monitor/monitor.c` — serial port open / read /
  DTR-RTS reset wiring (for the UART pane).
- `resolve_tool()`.
- `config_get()` for project keys.
- `esf_find_esp_port()` for port auto-detection.
- `process_start_pty()` for the gdb pane.
- All of `feat/tui-pane-rework`: `tui_log` self-contained inspect,
  `tui_status_bar`, mouse / paste / yank.

## Open items for phase 1

- **OpenOCD launcher placement**: inline in
  `cmd/target/debug/debug.c` vs. a separate `openocd.{c,h}` pair.
  v1 inline; factor out if a second user appears.
- **`debug_arguments_openocd` shape**: confirm with a real built
  project — single string vs. shell-tokenised list.  esp-idf seems
  to keep it as a single string with embedded args; we'll need to
  shell-split when feeding to argv.  `svec_split` or similar.
- **Daemon cleanup on SIGKILL**: process-group SIGTERM covers
  SIGTERM / SIGINT / normal exit.  SIGKILL leaves OpenOCD orphaned;
  document for MVP rather than work around.
- **Reset semantics**: `Ctrl-T+r` via gdb's `monitor reset halt`
  runs but leaves the CPU halted at reset.  User has to `continue`
  to resume.  Match the `qemu --debug` behaviour (which leaves
  halted via `monitor system_reset`)?  Or add a `Ctrl-T+R` for
  reset-and-run?  Defer this judgment call until we've used it.
- **DTR / RTS on the UART pane**: should `--no-reset` be supported?
  Some JTAG adapters share DTR / RTS with the UART (the esp-prog
  has separate lines).  Keep it explicit — copy the flag from
  `ice monitor`.

## Phase 1 estimate

~280-380 LOC across the new files.  ~5-7h with testing.
No new `platform.h` additions expected; `process_start`,
`serial_*`, `term_*` cover everything.

---

# Phase 2 — gdb-stub auto-attach in `ice monitor`

## Goal

When the firmware panics and ESP-IDF's built-in `esp_gdbstub` takes
over the UART, `ice monitor` notices, freezes the monitor, hands the
serial port to gdb, and lets the user inspect the panic.  When the
user quits gdb, monitor resumes.  No JTAG / OpenOCD required —
purely UART-based, in-band gdb-remote.

This is the killer convenience feature of the firmware stub — most
panics during development happen while the user is already
monitoring, and ice can pivot them straight into a debugger without
flashing or wiring anything.

## Why monitor and not `ice debug`

The trigger is reactive (the firmware decides), the transport is
the same UART monitor is already listening on, and the firmware
has to be built with `CONFIG_ESP_GDBSTUB_ENABLED` for any of this
to work.  Putting it in `ice debug` (which is a deliberate "I want
to debug" invocation) misses the point.

## How it works

Reference: `esp-idf-monitor/esp_idf_monitor/base/gdbhelper.py`,
~10 lines for the detection.

- **Detection**: byte-stream filter on the UART feed scans for
  `\$(T..)#(..)` — a gdb-remote stop-reason packet — with checksum
  validation.  Deterministic, no heuristics.  No false positives
  from normal printf output (the `$` is rare, the surrounding form
  is unique).
- **Mode switch** on match:
  1. Freeze the monitor's TUI.
  2. Close (or hand off) the serial port.
  3. Spawn `<gdb-prefix>gdb -ex "set serial baud <baud>" -ex
     "target remote <port>" <elf>` in the same alt-screen.
  4. Pump gdb in the foreground; show its session like a one-pane
     `qemu --debug`.  ESP gdbs (xtensa + riscv32) speak gdb-remote
     over a serial device directly — no TCP bridge needed.
- **Resume** on gdb exit:
  1. Reopen the serial port.
  2. Write `+$c#63` (ACK + `c` continue command in gdb-remote
     framing) to the port.  Firmware resumes from where it
     trapped; no reset needed.
  3. Unfreeze the monitor TUI.

## Implementation steps

### 2.1  Byte-stream filter primitive — `~50-80 LOC`

Defer-or-pull-forward depending on what's already in tree by the
time phase 2 starts.  PR #41's review discussion floated a
"byte-stream filter between `serial_read` and `vt100_input`" pattern
for future content transformations (addr2line, coredump).  This is
the first real user.

Sketch: `struct mon_filter` with `feed(in, out)` that may rewrite
or just pass-through.  Cmd composes a chain.  Inline in
`cmd/target/monitor/monitor.c` until a second filter appears.

### 2.2  gdb-stub detection filter — `~40 LOC`

Stateful filter: scan for `$T..#..` across chunk boundaries; on
match, compute the gdb-remote checksum (sum of payload bytes mod
256) and compare to the two hex digits after `#`; on validation,
fire a callback.

### 2.3  Mode-switch in monitor — `~80 LOC`

Cmd-level state machine: `LIVE` (current monitor state) ↔
`GDB_ATTACHED`.  Transitions:

- LIVE → GDB_ATTACHED on filter callback: freeze TUI, close serial,
  spawn gdb pty, route input/output between gdb pane and the
  serial-as-target-remote (gdb owns that fd directly via its
  built-in serial driver).
- GDB_ATTACHED → LIVE on gdb pty EOF: reopen serial, write
  `+$c#63`, unfreeze.

The TUI uses one pane in either state — don't fork into the
two-pane debug shape.  This keeps `ice monitor` one-pane in spirit;
users who want two panes can use `ice debug`.

### 2.4  Tests + manual verification

- Manual: build a project with `CONFIG_ESP_GDBSTUB_ENABLED=y`
  (default), introduce a deliberate panic, run `ice monitor`,
  confirm auto-attach.
- Verify the resume path: `quit` from gdb, observe firmware
  continue running, monitor pumping bytes again.

## Open items for phase 2

- **Auto-attach on/off**: should it be opt-in (`--gdb-stub`) or
  default-on?  Default-on is more useful but means a corrupt UART
  byte stream that happens to look like `$T..#..` could fake a
  panic-attach.  Checksum validation makes a false positive
  cosmically unlikely; default on, with `--no-gdb-stub` to disable
  if it ever bites.
- **ELF resolution**: `ice monitor` (porcelain) can use
  `_project.elf`.  `ice target monitor` (plumbing) can't unless the
  user passes `--elf`.  Question: do we wire gdb-stub auto-attach
  in both, or only in the porcelain?  Probably porcelain only, with
  `--elf` available on the plumbing as a manual override.
- **Multiple ELFs (bootloader, app)**: idf_monitor supports adding
  bootloader.elf via `add-symbol-file`.  Defer; single ELF for v1.

## Phase 2 estimate

~150-200 LOC.  ~3-4h.

---

# Reference docs

- ESP-IDF gdbstub: `components/esp_gdbstub/src/gdbstub.c`,
  `gdbstub_transport.c`, `packet.c`.
- esp-idf-monitor gdb path:
  `esp-idf-monitor/esp_idf_monitor/base/gdbhelper.py` (`run_gdb`,
  detection regex, resume bytes).
- esp-idf openocd launcher: `tools/idf_py_actions/debug_ext.py`
  (board-file selection, log polling).
- esp-idf openocd config gen: `tools/cmake/openocd.cmake`
  (`__get_openocd_options` — feeds `debug_arguments_openocd` into
  `project_description.json`).
- ice TUI baseline: PR #41 (`feat/tui-pane-rework`) — widget-owned
  inspect, mouse/paste/yank, dual-pane scaffold.
