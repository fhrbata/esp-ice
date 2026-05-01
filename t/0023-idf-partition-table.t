#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# Golden byte-diff: every `ice idf partition-table <subcmd>` against
# the corresponding ESP-IDF Python tool.  Skipped when the IDF
# components/partition_table directory or python3 are not available.

PT_DIR=""
for cand in \
    "$HOME/work/esp-idf/components/partition_table"; do
    if [ -f "$cand/gen_esp32part.py" ] && \
       [ -f "$cand/check_sizes.py" ] && \
       [ -f "$cand/gen_empty_partition.py" ] && \
       [ -f "$cand/gen_extra_subtypes_inc.py" ] && \
       [ -f "$cand/partitions_singleapp.csv" ] && \
       [ -f "$cand/partitions_two_ota.csv" ]; then
        PT_DIR="$cand"
        break
    fi
done

if [ -z "$PT_DIR" ] || ! command -v python3 >/dev/null 2>&1; then
    echo "1..0 # SKIP IDF partition_table sources or python3 not available"
    exit 0
fi

. t/tap.sh
tap_setup

ICE="$BINARY"
[ -z "$ICE" ] && ICE="$(dirname "$LIBICE")/ice"

# Stage canonical partition tables once.
python3 "$PT_DIR/gen_esp32part.py" "$PT_DIR/partitions_singleapp.csv" "$O/pt.bin" >/dev/null 2>&1
python3 "$PT_DIR/gen_esp32part.py" "$PT_DIR/partitions_two_ota.csv"   "$O/pt2.bin" >/dev/null 2>&1

# ---------------------------------------------------------------------- decode
python3 "$PT_DIR/gen_esp32part.py" "$O/pt.bin"  "$O/py_dec.csv"  >/dev/null 2>&1
"$ICE" idf partition-table decode "$O/pt.bin" > "$O/ice_dec.csv" 2>/dev/null
tap_check diff -u "$O/py_dec.csv" "$O/ice_dec.csv"
tap_done "decode singleapp PT matches gen_esp32part.py reverse"

python3 "$PT_DIR/gen_esp32part.py" "$O/pt2.bin" "$O/py_dec2.csv" >/dev/null 2>&1
"$ICE" idf partition-table decode "$O/pt2.bin" > "$O/ice_dec2.csv" 2>/dev/null
tap_check diff -u "$O/py_dec2.csv" "$O/ice_dec2.csv"
tap_done "decode two_ota PT matches gen_esp32part.py reverse"

# ---------------------------------------------------------------------- empty
python3 "$PT_DIR/gen_empty_partition.py" 0x1000 "$O/py_empty.bin"
"$ICE" idf partition-table empty 0x1000 "$O/ice_empty.bin"
tap_check cmp "$O/py_empty.bin" "$O/ice_empty.bin"
tap_done "empty 0x1000 matches gen_empty_partition.py"

python3 "$PT_DIR/gen_empty_partition.py" 4096 "$O/py_empty2.bin"
"$ICE" idf partition-table empty 4096 "$O/ice_empty2.bin"
tap_check cmp "$O/py_empty2.bin" "$O/ice_empty2.bin"
tap_done "empty 4096 (decimal) matches"

python3 "$PT_DIR/gen_empty_partition.py" 0x100 > "$O/py_empty3.bin"
"$ICE" idf partition-table empty 0x100 > "$O/ice_empty3.bin"
tap_check cmp "$O/py_empty3.bin" "$O/ice_empty3.bin"
tap_done "empty to stdout matches"

# ---------------------------------------------------------------- subtypes-header
python3 "$PT_DIR/gen_extra_subtypes_inc.py" "$O/py_h.h" \
    "fs,foo,0x40" "fs,bar,0x41"
"$ICE" idf partition-table subtypes-header "$O/ice_h.h" \
    "fs,foo,0x40" "fs,bar,0x41"
tap_check diff "$O/py_h.h" "$O/ice_h.h"
tap_done "subtypes-header with two custom subtypes matches"

python3 "$PT_DIR/gen_extra_subtypes_inc.py" "$O/py_h_empty.h"
"$ICE" idf partition-table subtypes-header "$O/ice_h_empty.h"
tap_check diff "$O/py_h_empty.h" "$O/ice_h_empty.h"
tap_done "subtypes-header with no extras matches"

# ----------------------------------------------------------------- check-bootloader
dd if=/dev/zero of="$O/bl_small.bin" bs=1024 count=8 status=none
python3 "$PT_DIR/check_sizes.py" bootloader 0x0 "$O/bl_small.bin" \
    > "$O/py_blok.out" 2> "$O/py_blok.err"
PY_RC=$?
"$ICE" idf partition-table check-bootloader 0x0 "$O/bl_small.bin" \
    > "$O/ice_blok.out" 2> "$O/ice_blok.err"
ICE_RC=$?
tap_check diff "$O/py_blok.out" "$O/ice_blok.out"
tap_done "check-bootloader (fits): stdout matches"
tap_check diff "$O/py_blok.err" "$O/ice_blok.err"
tap_done "check-bootloader (fits): stderr matches"
tap_check test "$PY_RC" -eq "$ICE_RC"
tap_done "check-bootloader (fits): exit code matches"

dd if=/dev/zero of="$O/bl_huge.bin" bs=1024 count=60 status=none
python3 "$PT_DIR/check_sizes.py" bootloader 0x0 "$O/bl_huge.bin" \
    > "$O/py_blbig.out" 2> "$O/py_blbig.err"
PY_RC=$?
"$ICE" idf partition-table check-bootloader 0x0 "$O/bl_huge.bin" \
    > "$O/ice_blbig.out" 2> "$O/ice_blbig.err"
ICE_RC=$?
tap_check diff "$O/py_blbig.out" "$O/ice_blbig.out"
tap_done "check-bootloader (overflow): stdout matches"
tap_check diff "$O/py_blbig.err" "$O/ice_blbig.err"
tap_done "check-bootloader (overflow): stderr matches"
tap_check test "$PY_RC" -eq "$ICE_RC"
tap_done "check-bootloader (overflow): exit code matches"

# ------------------------------------------------------------------ check-partition
for kb in 256 990 2000; do
    dd if=/dev/zero of="$O/app_${kb}k.bin" bs=1024 count=$kb status=none
    python3 "$PT_DIR/check_sizes.py" partition --type app \
        "$O/pt.bin" "$O/app_${kb}k.bin" \
        > "$O/py_${kb}.out" 2> "$O/py_${kb}.err"
    PY_RC=$?
    "$ICE" idf partition-table check-partition --type app \
        "$O/pt.bin" "$O/app_${kb}k.bin" \
        > "$O/ice_${kb}.out" 2> "$O/ice_${kb}.err"
    ICE_RC=$?
    tap_check diff "$O/py_${kb}.out" "$O/ice_${kb}.out"
    tap_done "check-partition ${kb}K: stdout matches"
    tap_check diff "$O/py_${kb}.err" "$O/ice_${kb}.err"
    tap_done "check-partition ${kb}K: stderr matches"
    tap_check test "$PY_RC" -eq "$ICE_RC"
    tap_done "check-partition ${kb}K: exit code matches"
done

# Multi-partition (two_ota) overflow path: Warning, mixed, exit 0.
python3 "$PT_DIR/check_sizes.py" partition --type app \
    "$O/pt2.bin" "$O/app_2000k.bin" \
    > "$O/py_two.out" 2> "$O/py_two.err"
PY_RC=$?
"$ICE" idf partition-table check-partition --type app \
    "$O/pt2.bin" "$O/app_2000k.bin" \
    > "$O/ice_two.out" 2> "$O/ice_two.err"
ICE_RC=$?
tap_check diff "$O/py_two.out" "$O/ice_two.out"
tap_done "check-partition two_ota all-overflow: stdout matches"
tap_check test "$PY_RC" -eq "$ICE_RC"
tap_done "check-partition two_ota all-overflow: exit code matches"

# Subtype filter.
python3 "$PT_DIR/check_sizes.py" partition --type app --subtype ota_0 \
    "$O/pt2.bin" "$O/app_256k.bin" \
    > "$O/py_sub.out" 2> "$O/py_sub.err"
"$ICE" idf partition-table check-partition --type app --subtype ota_0 \
    "$O/pt2.bin" "$O/app_256k.bin" \
    > "$O/ice_sub.out" 2> "$O/ice_sub.err"
tap_check diff "$O/py_sub.out" "$O/ice_sub.out"
tap_done "check-partition --subtype ota_0: stdout matches"

# No matching partitions.
python3 "$PT_DIR/check_sizes.py" partition --type bootloader \
    "$O/pt2.bin" "$O/app_256k.bin" \
    > "$O/py_nm.out" 2> "$O/py_nm.err"
"$ICE" idf partition-table check-partition --type bootloader \
    "$O/pt2.bin" "$O/app_256k.bin" \
    > "$O/ice_nm.out" 2> "$O/ice_nm.err"
tap_check diff "$O/py_nm.out" "$O/ice_nm.out"
tap_done "check-partition no-match: stdout matches"

tap_result
