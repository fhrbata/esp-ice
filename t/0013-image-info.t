#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# Smoke-test `ice image info` against an image we produced ourselves
# with `ice image create`.  Skipped when no ESP-IDF example ELF is
# available on the machine (same gate as the esptool golden-diff
# test) -- we need a real ESP-IDF ELF because info's most interesting
# output (chip auto-detection, app_desc parsing) only makes sense for
# images that actually carry that metadata.

ELF=""
for cand in \
    "$HOME/work/esp-idf/examples/get-started/hello_world/build/hello_world.elf" \
    "$HOME/work/esp-idf/examples/get-started/hello_world/build/bootloader/bootloader.elf"; do
    [ -f "$cand" ] && ELF="$cand" && break
done

if [ -z "$ELF" ]; then
    echo "1..0 # SKIP no ESP-IDF example ELF available"
    exit 0
fi

O="$T_OUT/$(basename "$0" .t)"
rm -rf "$O" && mkdir -p "$O"

ICE="$BINARY"
[ -z "$ICE" ] && ICE="$(dirname "$LIBICE")/ice"

BIN="$O/app.bin"
OUT="$O/info.txt"

"$ICE" image create --chip esp32 --flash-mode dio --flash-freq 40m \
    --flash-size 4MB --elf-sha256-offset 0xb0 -o "$BIN" "$ELF" \
    >/dev/null 2>&1 || { echo "Bail out! ice image create failed"; exit 1; }

"$ICE" image info "$BIN" > "$OUT" 2>&1 || {
    echo "Bail out! ice image info failed"
    cat "$OUT"
    exit 1
}

n=0
check() {
    local desc="$1" pattern="$2"
    n=$((n + 1))
    if grep -q -- "$pattern" "$OUT"; then
        echo "ok $n - $desc"
    else
        echo "not ok $n - $desc (missing: $pattern)"
        fail=1
    fi
}

fail=0
check "reports image magic 0xe9"            'Magic .*: 0xe9'
check "auto-detects chip from chip_id"      'Chip ID .*(esp32)'
check "flash mode echoed back"              'Flash mode .*: dio'
check "flash size echoed back"              'Flash size .*: 4MB'
check "flash freq echoed back"              'Flash freq .*: 40m'
check "lists segments with regions"         'DROM\|IROM\|DRAM\|IRAM'
check "checksum section present"            '^Checksum:'
check "one or more sections marked valid"   'Result.*valid'
check "SHA-256 digest header present"       'SHA-256 digest'
check "application information block"       'Application information'

echo "1..$n"
[ "$fail" -eq 0 ]
