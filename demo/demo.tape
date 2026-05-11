# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# demo/demo.tape -- end-to-end walkthrough for the README.
#
# Drives ice from a fresh checkout of esp-idf master through the
# hello_world example: completion -> repo checkout -> init ->
# menuconfig -> build -> flash -> monitor -> qemu -> qemu --debug ->
# ice debug.  Real chip + real QEMU + real OpenOCD; render this on
# the host the chip is wired to.
#
# Render with vhs (https://github.com/charmbracelet/vhs):
#   cd ~/work/esp-ice && vhs demo/demo.tape
#
# Requires:
#   - vhs, ttyd, ffmpeg
#   - ice on PATH
#   - ESP32-S3 attached on /dev/ttyUSB0 (UART) and /dev/ttyACM0 (USB-JTAG)
#   - ~/.ice/esp-idf reference already populated; do at least one
#     `ice repo checkout master /tmp/warm` before recording so the
#     submodule packs are cached -- otherwise step 2 will record several
#     minutes of cloning into the GIF.
#
# All steps run in real time -- nothing is hidden.  USB-JTAG is used
# for flashing because it's faster than UART.
#
# Progress-driven steps (repo checkout, init, build, flash) sync via
# `Wait+Screen /<msg> done/` so the next Type fires only after ice has
# fully released stdin -- vhs's wall-clock Sleep otherwise outruns the
# operation, and bytes typed while ice is still in raw mode (polling
# stdin for the Ctrl-V verbose toggle) get eaten before bash regains
# control.  +Screen (vs the default +Line) is needed because the bash
# prompt overwrites the last line right after ice's "✓ <msg> done."
# success line, racing the matcher.

Output demo/demo.gif

Set Shell        bash
Set FontFamily   "Adwaita Mono"
Set FontSize     13
Set Width        1100
Set Height       720
Set Padding      14
Set Theme { "name": "ice-demo", "background": "#000000", "foreground": "#E5E5E5", "cursor": "#FFFFFF", "selection": "#444444", "black": "#000000", "red": "#CC0000", "green": "#4E9A06", "yellow": "#C4A000", "blue": "#3465A4", "magenta": "#75507B", "cyan": "#06989A", "white": "#D3D7CF", "brightBlack": "#555753", "brightRed": "#EF2929", "brightGreen": "#8AE234", "brightYellow": "#FCE94F", "brightBlue": "#729FCF", "brightMagenta": "#AD7FA8", "brightCyan": "#34E2E2", "brightWhite": "#EEEEEC" }
Set TypingSpeed  50ms

# Start in demo/, throw away any previous esp-idf checkout, clean prompt.
Hide
Type "cd demo && rm -rf esp-idf"
Enter
Type "export PS1='$ '"
Enter
Type "clear"
Enter
Sleep 1000ms
Show

# ---------------------------------------------------------------------------
# 1. Enable tab completion.
# ---------------------------------------------------------------------------
Type "# enable tab completion"
Sleep 1000ms
Enter
Type `eval "$(ice completion bash)"`
Sleep 1000ms
Enter
Sleep 800ms

# ---------------------------------------------------------------------------
# 2. Local checkout of esp-idf master (clones from ~/.ice/esp-idf).
# ---------------------------------------------------------------------------
Type "# check out esp-idf master into ./esp-idf"
Sleep 1000ms
Enter
Type "ice repo checkout master ./esp-idf"
Sleep 1000ms
Enter
Wait+Screen@60s /Checking out done/
Sleep 1000ms

# ---------------------------------------------------------------------------
# 3. Hop into the hello_world example.
# ---------------------------------------------------------------------------
Type "# move into the hello_world example"
Sleep 1000ms
Enter
Type "cd esp-idf/examples/get-started/hello_world/"
Sleep 1000ms
Enter
Sleep 800ms

# ---------------------------------------------------------------------------
# 4. Bind the project to esp32s3 + the local esp-idf checkout (../../..
#    from hello_world/).
# ---------------------------------------------------------------------------
Type "# bind the project to esp32-s3 and the local esp-idf checkout"
Sleep 1000ms
Enter
Type "ice init esp32s3 ../../.."
Sleep 1000ms
Enter
Wait+Screen@60s /Configuring done/
Sleep 1000ms

# ---------------------------------------------------------------------------
# 5. menuconfig -- ice's native Kconfig TUI.  Navigate, peek at help,
#    quit without saving so the example's sdkconfig stays as init left it.
# ---------------------------------------------------------------------------
Type "# launch menuconfig (ice's native Kconfig TUI)"
Sleep 1000ms
Enter
Type "ice menuconfig"
Sleep 1000ms
Enter
Sleep 2500ms
Down
Sleep 250ms
Down
Sleep 250ms
Down
Sleep 250ms
Down
Sleep 350ms
Enter
Sleep 1500ms
Type "?"
Sleep 2500ms
Escape
Sleep 500ms
Escape
Sleep 500ms
Type "q"
Sleep 1500ms

# ---------------------------------------------------------------------------
# 6. ice build.  Spinner-only -- Ctrl-V verbose toggle works interactively
#    but tends to overwhelm vhs's screen tracking when ninja prints
#    hundreds of lines through it.
# ---------------------------------------------------------------------------
Type "# build the firmware"
Sleep 1000ms
Enter
Type "ice build"
Sleep 1000ms
Enter
# Wait for the build to finish.
Wait+Screen@5m /Building done/
# Linger on the ✓ Building done. (...) line.
Sleep 2000ms

# ---------------------------------------------------------------------------
# 7. ice flash via USB-JTAG -- much faster than UART on this image.
# ---------------------------------------------------------------------------
Type "# flash via USB-JTAG (faster than UART on this image)"
Sleep 1000ms
Enter
Type "ice flash --port /dev/ttyACM0"
Sleep 1000ms
Enter
# Wait for flash to finish, then linger on ✓ Flashing done.
Wait+Screen@60s /Flashing done/
Sleep 2000ms

# ---------------------------------------------------------------------------
# 8. Monitor -- live UART through ice's vt100 pipeline.  Ctrl-] exits.
# ---------------------------------------------------------------------------
Type "# attach the serial monitor (Ctrl-] exits)"
Sleep 1000ms
Enter
Type "ice monitor"
Sleep 1000ms
Enter
Sleep 5000ms
Ctrl+]
Sleep 1000ms

# ---------------------------------------------------------------------------
# 9. QEMU -- run the same firmware in qemu-system-xtensa.  Ctrl-T x exits.
# ---------------------------------------------------------------------------
Type "# run the firmware under QEMU (Ctrl-T x exits)"
Sleep 1000ms
Enter
Type "ice qemu"
Sleep 1000ms
Enter
Sleep 5000ms
Ctrl+T
Sleep 100ms
Type "x"
Sleep 1500ms

# ---------------------------------------------------------------------------
# 10. QEMU --debug -- gdb on top, UART on the bottom.  QEMU starts paused;
#     `c` in the gdb pane releases the chip and the UART pane fills with the
#     boot log.
# ---------------------------------------------------------------------------
Type "# QEMU + gdb dual-pane (c continues, Ctrl-T x exits)"
Sleep 1000ms
Enter
Type "ice qemu --debug"
Sleep 1000ms
Enter
Sleep 3000ms
Type "c"
Sleep 1000ms
Enter
Sleep 5000ms
Ctrl+T
Sleep 100ms
Type "x"
Sleep 1500ms

# ---------------------------------------------------------------------------
# 11. Real debug -- OpenOCD + xtensa-esp-elf-gdb against the chip.
#     Reset, set a breakpoint at app_main, continue, backtrace at the hit.
# ---------------------------------------------------------------------------
Type "# debug the real chip via OpenOCD + gdb"
Sleep 1000ms
Enter
Type "ice debug"
Sleep 1000ms
Enter
# OpenOCD spawns + gdb attaches and halts the chip.
Sleep 3000ms

# Break on app_main.
Type "b app_main"
Sleep 1000ms
Enter

# Continue -- bootloader runs, app starts, breakpoint hits at app_main entry.
Type "c"
Sleep 1000ms
Enter
Sleep 11000ms

# Show the backtrace at the hit.
Type "bt"
Sleep 1000ms
Enter
Sleep 2000ms

# Exit the dual-pane TUI.
Ctrl+T
Sleep 100ms
Type "x"
