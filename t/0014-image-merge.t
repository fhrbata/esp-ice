#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# Golden byte-diff: `ice image merge` vs `esptool merge-bin` on a real
# ESP-IDF build directory's bootloader + partition table + app.
# Skipped when esptool or an IDF build are not available.

HW=""
for cand in \
    "$HOME/work/esp-idf/examples/get-started/hello_world/build"; do
    if [ -f "$cand/bootloader/bootloader.bin" ] && \
       [ -f "$cand/partition_table/partition-table.bin" ] && \
       [ -f "$cand/hello_world.bin" ]; then
        HW="$cand"
        break
    fi
done

if [ -z "$HW" ] || ! command -v esptool >/dev/null 2>&1; then
    echo "1..0 # SKIP esptool or IDF build artifacts not available"
    exit 0
fi

O="$T_OUT/$(basename "$0" .t)"
rm -rf "$O" && mkdir -p "$O"

ICE="$BINARY"
[ -z "$ICE" ] && ICE="$(dirname "$LIBICE")/ice"

BOOT="$HW/bootloader/bootloader.bin"
PT="$HW/partition_table/partition-table.bin"
APP="$HW/hello_world.bin"

pass=0
fail=0
n=0
check() {
    local desc="$1"; shift
    n=$((n + 1))
    "$ICE" image merge -o "$O/ice.bin" "$@" >/dev/null 2>&1 || {
        echo "not ok $n - ice merge failed: $desc"; fail=$((fail + 1)); return
    }
    esptool --chip esp32 merge-bin -o "$O/esptool.bin" "$@" \
        >/dev/null 2>&1 || {
        echo "not ok $n - esptool merge-bin failed: $desc"
        fail=$((fail + 1)); return
    }
    if cmp -s "$O/ice.bin" "$O/esptool.bin"; then
        echo "ok $n - $desc"
        pass=$((pass + 1))
    else
        echo "not ok $n - differs from esptool: $desc"
        fail=$((fail + 1))
    fi
}

# Basic three-file merge (no padding).
check "basic 3-file merge" \
    0x0 "$BOOT" 0x8000 "$PT" 0x10000 "$APP"

# Padded to 4MB.
check "merge with --pad-to-size 4MB" \
    --pad-to-size 4MB \
    0x0 "$BOOT" 0x8000 "$PT" 0x10000 "$APP"

# Reversed input order (should still place each file at its offset).
check "file order does not affect output" \
    0x10000 "$APP" 0x0 "$BOOT" 0x8000 "$PT"

echo "1..$n"
[ "$fail" -eq 0 ]
