#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# Offline tests for `ice target partition`: selector resolution
# against a CSV/binary partition table file (no device required).
# Skipped when the IDF partition_table sources are not available.

PT_DIR=""
for cand in \
    "$HOME/work/esp-idf/components/partition_table"; do
    if [ -f "$cand/gen_esp32part.py" ] && \
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

python3 "$PT_DIR/gen_esp32part.py" "$PT_DIR/partitions_singleapp.csv" "$O/pt.bin"  >/dev/null 2>&1
python3 "$PT_DIR/gen_esp32part.py" "$PT_DIR/partitions_two_ota.csv"   "$O/pt2.bin" >/dev/null 2>&1

run() {
    local out="$O/$1"; shift
    "$ICE" target partition "$@" --partition-table-file "$O/pt.bin" \
        > "$out" 2>&1
}

run2() {
    local out="$O/$1"; shift
    "$ICE" target partition "$@" --partition-table-file "$O/pt2.bin" \
        > "$out" 2>&1
}

# --------------------------------------------------------------------- info
run info_default.out info --name nvs
echo "0x9000 0x6000" > "$O/expected_info_default.out"
tap_check diff "$O/expected_info_default.out" "$O/info_default.out"
tap_done "info --name: default fields are 'offset size'"

run info_named.out info --name nvs --info name --info offset --info size
echo "nvs 0x9000 0x6000" > "$O/expected_info_named.out"
tap_check diff "$O/expected_info_named.out" "$O/info_named.out"
tap_done "info --name --info name --info offset --info size"

run info_bytype.out info --type app --info name --info subtype
echo "factory 0" > "$O/expected_info_bytype.out"
tap_check diff "$O/expected_info_bytype.out" "$O/info_bytype.out"
tap_done "info --type app --info name --info subtype: first match"

# Boot-default: factory present.
run info_boot_factory.out info --boot-default
echo "0x10000 0x100000" > "$O/expected_info_boot_factory.out"
tap_check diff "$O/expected_info_boot_factory.out" "$O/info_boot_factory.out"
tap_done "info --boot-default: returns factory when present"

# Boot-default: no factory (build a CSV without one).
cat > "$O/no_factory.csv" <<'EOF'
nvs, data, nvs, 0x9000, 0x4000,
otadata, data, ota, 0xd000, 0x2000,
ota_0, app, ota_0, 0x10000, 1M,
ota_1, app, ota_1, 0x110000, 1M,
EOF
"$ICE" target partition info --partition-table-file "$O/no_factory.csv" \
    --boot-default --info name > "$O/info_boot_ota0.out" 2>&1
echo "ota_0" > "$O/expected_info_boot_ota0.out"
tap_check diff "$O/expected_info_boot_ota0.out" "$O/info_boot_ota0.out"
tap_done "info --boot-default: falls through to ota_0 when no factory"

# --part_list with --type app on two_ota PT (3 matches).
run2 info_partlist.out info --type app --part_list \
    --info name --info offset
{
    echo "factory 0x10000 ota_0 0x110000 ota_1 0x210000"
} > "$O/expected_info_partlist.out"
tap_check diff "$O/expected_info_partlist.out" "$O/info_partlist.out"
tap_done "info --part_list: every matching partition is printed"

# --part_list error path: no --type.
"$ICE" target partition info --partition-table-file "$O/pt2.bin" \
    --part_list --name nvs > "$O/info_partlist_err.out" 2>&1
tap_check ! test $? -eq 0
tap_done "info --part_list without --type: rejected"

# --extra-partition-subtypes registers a custom subtype name.
cat > "$O/custom.csv" <<'EOF'
nvs, data, nvs, 0x9000, 0x6000,
custom1, data, foo, 0xf000, 0x1000,
EOF

# Without the flag, parsing fails on the unknown subtype.
"$ICE" idf partition-table "$O/custom.csv" "$O/custom_bad.bin" \
    > "$O/custom_bad.out" 2>&1
tap_check ! test $? -eq 0
tap_done "custom subtype is rejected by csv parser without --extra-partition-subtypes"

# With the flag (via target partition info), it's recognised.
"$ICE" target partition info --partition-table-file "$O/custom.csv" \
    --name custom1 --extra-partition-subtypes data,foo,0x80 \
    > "$O/custom_ok.out" 2>&1
echo "0xf000 0x1000" > "$O/expected_custom_ok.out"
tap_check diff "$O/expected_custom_ok.out" "$O/custom_ok.out"
tap_done "info with --extra-partition-subtypes: recognises custom name"

# --esptool-args family rejected with a useful diagnostic.
for flag in --esptool-args --esptool-write-args --esptool-read-args \
            --esptool-erase-args; do
    "$ICE" target partition info --partition-table-file "$O/pt.bin" \
        --name nvs $flag "--anything" > "$O/reject.out" 2>&1
    rc=$?
    tap_check test "$rc" -ne 0
    tap_check grep -q "esp-serial-flasher" "$O/reject.out"
    tap_done "$flag is rejected with esp-serial-flasher hint"
done

tap_result
