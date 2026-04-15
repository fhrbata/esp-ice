#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# Golden byte-diff: `ice image create` vs `esptool elf2image
# --use-segments` on a real ESP-IDF ELF.  Skipped if esptool is not on
# PATH or no pre-built IDF example ELF is present on this machine --
# the image engine also has a self-contained structural test at
# t/0011-image-create.t that runs unconditionally.

# Locate a usable ELF.
ELF=""
for cand in \
    "$HOME/work/esp-idf/examples/get-started/hello_world/build/hello_world.elf" \
    "$HOME/work/esp-idf/examples/get-started/hello_world/build/bootloader/bootloader.elf" \
    "$HOME/work/esp-idf/examples/wifi/getting_started/station/build/wifi_station.elf"; do
    [ -f "$cand" ] && ELF="$cand" && break
done

if [ -z "$ELF" ] || ! command -v esptool >/dev/null 2>&1; then
    echo "1..0 # SKIP esptool or IDF example ELF not available"
    exit 0
fi

O="$T_OUT/$(basename "$0" .t)"
rm -rf "$O" && mkdir -p "$O"

# "ice" binary: $BINARY is exported by the test harness; fall back to
# deriving the path from $LIBICE so a plain `prove` invocation also
# works.
ICE="$BINARY"
[ -z "$ICE" ] && ICE="$(dirname "$LIBICE")/ice"

pass=0
fail=0
n=0
check() {
    local desc="$1" ff="$2" fs="$3" fm="$4"
    n=$((n + 1))
    "$ICE" image create --chip esp32 --flash-mode "$fm" \
        --flash-freq "$ff" --flash-size "$fs" --elf-sha256-offset 0xb0 \
        -o "$O/ice.bin" "$ELF" >/dev/null 2>&1 || {
            echo "not ok $n - ice failed: $desc"; fail=$((fail + 1)); return
        }
    esptool --chip esp32 elf2image --use-segments \
        --flash-mode "$fm" --flash-freq "$ff" --flash-size "$fs" \
        --elf-sha256-offset 0xb0 \
        -o "$O/esptool.bin" "$ELF" >/dev/null 2>&1 || {
            echo "not ok $n - esptool failed: $desc"; fail=$((fail + 1)); return
        }
    if cmp -s "$O/ice.bin" "$O/esptool.bin"; then
        echo "ok $n - $desc"
        pass=$((pass + 1))
    else
        echo "not ok $n - differs from esptool: $desc"
        fail=$((fail + 1))
    fi
}

# Cover a handful of {flash_mode, flash_freq, flash_size} combinations.
check "dio/40m/2MB" 40m 2MB dio
check "dio/80m/4MB" 80m 4MB dio
check "qio/40m/4MB" 40m 4MB qio
check "qio/80m/8MB" 80m 8MB qio
check "dout/40m/16MB" 40m 16MB dout
check "qout/26m/4MB" 26m 4MB qout

echo "1..$n"
[ "$fail" -eq 0 ]
