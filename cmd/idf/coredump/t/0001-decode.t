#!/usr/bin/env bash
#
# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
#
# SPDX-License-Identifier: Apache-2.0
#
# End-to-end tests for `ice idf coredump`: input format detection and
# the b64 -> raw decode path (--save-core round-trip).

. t/tap.sh
tap_setup

# ---- Build a deterministic 200-byte binary "core image" ----

# LC_ALL=C: in UTF-8 locales awk's %c expands high bytes into multi-byte
# sequences, breaking the size invariant we want for a binary fixture.
LC_ALL=C awk 'BEGIN { for (i = 0; i < 200; i++) printf("%c", i % 256) }' >original.bin
tap_check test "$(wc -c <original.bin)" -eq 200
tap_done "fixture is 200 bytes"

# ---- 1. b64 round-trip, single-line (no internal padding) ----

base64 -w 0 original.bin >single.b64 2>/dev/null || base64 original.bin | tr -d '\n' >single.b64
"$BINARY" idf coredump --core single.b64 --core-format b64 --save-raw out_single.bin
tap_check cmp -s original.bin out_single.bin
tap_done "b64 single-line decodes to original bytes"

# ---- 2. b64 round-trip, line-wrapped (76-col PEM-style) ----
# 200 bytes -> 268 b64 chars -> three 76-col lines + a 40-char tail.
# Some of those lines can end in '=' padding internally, which is the
# whole point of decoding line-by-line rather than as one stream.

base64 original.bin >wrapped.b64
tap_check test "$(wc -l <wrapped.b64)" -ge 2
"$BINARY" idf coredump --core wrapped.b64 --core-format b64 --save-raw out_wrapped.bin
tap_check cmp -s original.bin out_wrapped.bin
tap_done "b64 line-wrapped decodes to original bytes"

# ---- 3. Multi-line b64 with deliberate mid-stream '==' padding ----
# Encode in two 16-byte chunks; each chunk's b64 ends in '==', so a
# naive concatenated decode would stop at the first '==' and lose the
# second half.

LC_ALL=C awk 'BEGIN { for (i = 0; i < 16; i++) printf("%c", i) }' >chunkA.bin
LC_ALL=C awk 'BEGIN { for (i = 16; i < 32; i++) printf("%c", i) }' >chunkB.bin
{
	base64 -w 0 chunkA.bin 2>/dev/null || base64 chunkA.bin | tr -d '\n'
	echo
	base64 -w 0 chunkB.bin 2>/dev/null || base64 chunkB.bin | tr -d '\n'
	echo
} >chunked.b64
cat chunkA.bin chunkB.bin >chunked.expected
"$BINARY" idf coredump --core chunked.b64 --core-format b64 \
	--save-raw out_chunked.bin
tap_check cmp -s chunked.expected out_chunked.bin
tap_done "b64 chunks separated by '==' newline boundaries decode correctly"

# ---- 4. ELF passthrough: --core-format=elf copies bytes through ----
# Use a real ELF file (the ice binary itself) so the magic bytes match.

cp "$BINARY" some.elf
"$BINARY" idf coredump --core some.elf --core-format elf --save-raw out_elf.bin
tap_check cmp -s some.elf out_elf.bin
tap_done "elf format is copied through unchanged"

# ---- 5. Auto detection: \x7fELF prefix selects 'elf' ----

"$BINARY" idf coredump --core some.elf --save-raw out_auto_elf.bin
tap_check cmp -s some.elf out_auto_elf.bin
tap_done "auto-detect: \\x7fELF prefix routes to elf passthrough"

# ---- 6. Auto detection: anything else falls through to b64 ----

"$BINARY" idf coredump --core wrapped.b64 --save-raw out_auto_b64.bin
tap_check cmp -s original.bin out_auto_b64.bin
tap_done "auto-detect: non-ELF input is decoded as b64"

# ---- 7. Raw passthrough (must be selected explicitly) ----

"$BINARY" idf coredump --core original.bin --core-format raw \
	--save-raw out_raw.bin
tap_check cmp -s original.bin out_raw.bin
tap_done "raw format is copied through unchanged"

# ---- 8. Missing --core is an error ----

if "$BINARY" idf coredump --save-core /tmp/should_not_exist >out 2>err; then
	tap_check false
else
	tap_check grep -q -- '--core' err
fi
tap_done "missing --core is rejected"

# ---- 9. Invalid b64 is rejected ----

printf 'not!valid#base64@\n' >bad.b64
if "$BINARY" idf coredump --core bad.b64 --core-format b64 \
	--save-core /tmp/should_not_exist >out 2>err; then
	tap_check false
else
	tap_check grep -qi 'invalid base64' err
fi
tap_done "invalid base64 is rejected"

# ---- 10. Info summary for a synthesized V2 raw image ----
# 20-byte V2 header + 8 bytes of data + 4 bytes CRC = 32-byte image.
# chip_ver=5 (esp32c3), dump_ver=BIN_V2 (0x0002).  Compute the CRC
# correctly so the checksum trailer verifies.

python3 - <<'PY' >v2.bin
import struct, sys, zlib
TOT = 32
hdr = struct.pack('<5I', TOT, (5 << 16) | 0x0002, 4, 196, 3)
data = b'\xaa' * 8
crc = zlib.crc32(hdr + data) & 0xffffffff
sys.stdout.buffer.write(hdr + data + struct.pack('<I', crc))
PY
"$BINARY" idf coredump --core v2.bin >v2_info 2>v2_err
tap_check grep -q 'format *raw' v2_info
tap_check grep -q 'BIN_V2' v2_info
tap_check grep -q 'esp32c3' v2_info
tap_check grep -q 'tasks *4' v2_info
tap_check grep -q 'segments *3' v2_info
tap_check grep -q 'CRC32.*\[verified\]' v2_info
tap_done "info summary for a V2 raw image, CRC verifies"

# ---- 10a. Tampered image: CRC32 mismatch should be reported and exit non-zero ----

python3 - <<'PY' >v2_bad.bin
import struct, sys, zlib
TOT = 32
hdr = struct.pack('<5I', TOT, (5 << 16) | 0x0002, 4, 196, 3)
data = b'\xaa' * 8
crc = zlib.crc32(hdr + data) & 0xffffffff
buf = bytearray(hdr + data + struct.pack('<I', crc))
buf[24] ^= 0xff   # tamper one data byte
sys.stdout.buffer.write(bytes(buf))
PY
if "$BINARY" idf coredump --core v2_bad.bin >bad_out 2>err; then
	tap_check false
else
	tap_check grep -q 'CRC32.*\[FAILED\]' bad_out
	tap_check grep -q 'checksum mismatch' bad_out
fi
tap_done "tampered V2 image is reported as [FAILED] with exit 1"

# ---- 10b. --no-verify skips the check ----

"$BINARY" idf coredump --core v2_bad.bin --no-verify >nv_out 2>err
tap_check grep -q 'skipped: --no-verify' nv_out
tap_done "--no-verify skips the checksum check and exits 0"

# ---- 10b. Auto-detection picks 'raw' from the version word ----

"$BINARY" idf coredump --core v2.bin --core-format auto >v2_auto 2>err
tap_check grep -q 'format *raw' v2_auto
tap_done "auto-detect identifies raw via dump_ver"

# ---- 10c. ELF input prints minimal info (no header to parse) ----

"$BINARY" idf coredump --core some.elf >elf_info 2>err
tap_check grep -q 'format *elf' elf_info
tap_done "elf input prints a minimal info block"

# ---- 10d. b64 input is decoded then header-parsed ----
# Wrap our V2 raw image in base64 and verify the info comes out the same.

base64 v2.bin >v2.b64
"$BINARY" idf coredump --core v2.b64 --core-format b64 >v2b64_info 2>err
tap_check grep -q 'format *b64' v2b64_info
tap_check grep -q 'BIN_V2' v2b64_info
tap_done "b64 input is decoded and the resulting raw header is parsed"

# ---- 11. Unknown --core-format is rejected ----

if "$BINARY" idf coredump --core wrapped.b64 --core-format bogus \
	--save-raw /tmp/should_not_exist >out 2>err; then
	tap_check false
else
	tap_check grep -q "unknown --core-format" err
fi
tap_done "unknown --core-format is rejected"

# ---- 12. --save-core extracts embedded ELF for ELF_SHA256_V2_2 ----
# Build a synthetic V2_2 image whose data section is a known 16-byte
# "ELF" payload with valid SHA256 trailer.  --save-core should write
# exactly those 16 bytes.

python3 - <<'PY' >v22.bin 2>v22.expected.tmp
import struct, sys, hashlib
elf_data = b'\x7fELF' + bytes(b for b in range(4, 16))   # 16 bytes
hdr = struct.pack('<3I', 12 + len(elf_data) + 32,
                  (13 << 16) | 0x0104,  # esp32c6, ELF_SHA256_V2_2
                  0x100)                # chip_rev
sha = hashlib.sha256(hdr + elf_data).digest()
sys.stdout.buffer.write(hdr + elf_data + sha)
sys.stderr.buffer.write(elf_data)
PY
mv v22.expected.tmp v22_elf.expected
"$BINARY" idf coredump --core v22.bin --save-core v22_extracted.elf >info 2>err
tap_check grep -q 'ELF_SHA256_V2_2' info
tap_check grep -q 'SHA256.*\[verified\]' info
tap_check cmp -s v22_elf.expected v22_extracted.elf
tap_done "--save-core extracts embedded ELF for ELF_SHA256_V2_2 dumps"

# ---- 13. --save-core errors out for BIN_V* dumps (synthesis missing) ----

if "$BINARY" idf coredump --core v2.bin --save-core /tmp/should_not_exist \
	>info 2>err; then
	tap_check false
else
	tap_check grep -q 'ELF synthesis' err
	tap_check grep -q -- '--save-raw' err
fi
tap_done "--save-core on a BIN_V* dump errors with a synthesis-TODO message"

# ---- 14. --save-core for elf-format input copies the file through ----

"$BINARY" idf coredump --core some.elf --save-core out_savecore_elf.bin >info 2>err
tap_check cmp -s some.elf out_savecore_elf.bin
tap_done "--save-core for elf-format input passes the file through"

# ---- 15. <prog> without --save-core: temp file is allocated, gdb runs, then it is unlinked ----
# Use a separate fake-gdb that writes the --core arg + an existence
# probe so we can verify (a) the file existed during the gdb run
# and (b) it's gone afterwards.  Point TMPDIR at a private dir so
# we know the path lives under our isolated test scratch.

mkdir -p tmp_dir
cat >fake-gdb-tempcheck <<'EOF'
#!/usr/bin/env bash
while [ "$#" -gt 0 ]; do
	if [ "$1" = "--core" ] && [ -n "$2" ]; then
		printf 'core=%s\n' "$2" >>tempcheck.log
		if [ -f "$2" ]; then
			printf 'exists=1\n' >>tempcheck.log
		else
			printf 'exists=0\n' >>tempcheck.log
		fi
		break
	fi
	shift
done
exit 0
EOF
chmod +x fake-gdb-tempcheck
rm -f tempcheck.log

TMPDIR="$PWD/tmp_dir" "$BINARY" idf coredump --core v22.bin \
	--gdb "$PWD/fake-gdb-tempcheck" /tmp/fake_prog.elf >out 2>err
tap_check grep -q '^core=' tempcheck.log
tap_check grep -q '^exists=1$' tempcheck.log
core_path="$(sed -n 's/^core=//p' tempcheck.log | head -1)"
tap_check test -n "$core_path"
tap_check test ! -e "$core_path"
case "$core_path" in
"$PWD/tmp_dir/"*) tap_check true ;;
*) tap_check false ;;
esac
tap_done "<prog> without --save-core uses TMPDIR temp file, cleaned after gdb"

# ---- 16. <prog> with --save-core spawns gdb, args reach the binary ----
# Use a fake gdb that writes its argv to a known file so we can
# inspect what ice actually invoked.

cat >fake-gdb <<'EOF'
#!/usr/bin/env bash
# Record argv for the test harness, then print a marker so stdout
# capture has something to grep for.
i=0
for a in "$@"; do
	printf 'argv[%d]=%s\n' "$i" "$a" >>fake-gdb.log
	i=$((i + 1))
done
echo "fake-gdb: ran"
exit 0
EOF
chmod +x fake-gdb
rm -f fake-gdb.log

"$BINARY" idf coredump --core v22.bin --save-core v22_run.elf \
	--gdb "$PWD/fake-gdb" /tmp/fake_prog.elf >gdb_out 2>gdb_err
tap_check grep -q 'fake-gdb: ran' gdb_out
# Bash's "$@" starts at $1, so the script sees argv[0]=--batch
# (gdb's own argv[0] is the program path, not visible in "$@").
tap_check grep -qx 'argv\[0\]=--batch' fake-gdb.log
tap_check grep -qx 'argv\[1\]=--quiet' fake-gdb.log
tap_check grep -qx 'argv\[8\]=--core' fake-gdb.log
tap_check grep -qx 'argv\[9\]=v22_run.elf' fake-gdb.log
tap_check grep -qx 'argv\[10\]=/tmp/fake_prog.elf' fake-gdb.log
tap_check grep -qx 'argv\[12\]=info threads' fake-gdb.log
tap_check grep -qx 'argv\[14\]=thread apply all bt' fake-gdb.log
tap_done "<prog> with --save-core spawns gdb with the expected argv"

# ---- 17. --gdb path that doesn't exist is reported, not a crash ----

if "$BINARY" idf coredump --core v22.bin --save-core v22_run2.elf \
	--gdb /nonexistent/gdb /tmp/fake_prog.elf >out 2>err; then
	tap_check false
else
	tap_check grep -q 'gdb' err
fi
tap_done "missing --gdb binary is reported gracefully"

# ---- 17b. --rom-elf passes 'add-symbol-file' through to gdb's argv ----

touch fake-rom.elf
rm -f fake-gdb.log
"$BINARY" idf coredump --core v22.bin --save-core v22_run3.elf \
	--gdb "$PWD/fake-gdb" --rom-elf "$PWD/fake-rom.elf" \
	/tmp/fake_prog.elf >gdb_rom_out 2>gdb_rom_err
tap_check grep -qx "argv\\[11\\]=-ex" fake-gdb.log
tap_check grep -qx "argv\\[12\\]=add-symbol-file $PWD/fake-rom.elf" \
	fake-gdb.log
# After --rom-elf inserts two argv entries, info-threads / bt indices
# shift by 2.
tap_check grep -qx 'argv\[14\]=info threads' fake-gdb.log
tap_check grep -qx 'argv\[16\]=thread apply all bt' fake-gdb.log
tap_done "--rom-elf passes 'add-symbol-file <path>' to gdb"

# ---- 17c. --rom-elf with a nonexistent path errors out before spawning gdb ----

rm -f fake-gdb.log
if "$BINARY" idf coredump --core v22.bin --save-core v22_run4.elf \
	--gdb "$PWD/fake-gdb" --rom-elf /nonexistent/rom.elf \
	/tmp/fake_prog.elf >out 2>err; then
	tap_check false
else
	tap_check grep -q -- '--rom-elf' err
	tap_check grep -qi 'no such file' err
	tap_check test ! -e fake-gdb.log
fi
tap_done "missing --rom-elf path is rejected before gdb is spawned"

# ---- 17d. --interactive strips --batch / --quiet / --nh and the canned commands ----

rm -f fake-gdb.log
"$BINARY" idf coredump --core v22.bin --save-core v22_run5.elf \
	--gdb "$PWD/fake-gdb" --interactive /tmp/fake_prog.elf >out 2>err
# Default mode argv: --batch / --quiet / --nh / -ex 'set pagination off' /
# -ex 'set print pretty on' / --core / <core> / <prog> / -ex 'info threads' /
# ... -- none of those should be present in the interactive argv except
# --core / <core> / <prog> / --nx.
tap_check ! grep -q -x 'argv\[.\]=--batch' fake-gdb.log
tap_check ! grep -q -x 'argv\[.\]=--quiet' fake-gdb.log
tap_check ! grep -q -x 'argv\[.\]=--nh' fake-gdb.log
tap_check ! grep -q 'set pagination off' fake-gdb.log
tap_check ! grep -q 'set print pretty on' fake-gdb.log
tap_check ! grep -q 'info threads' fake-gdb.log
tap_check ! grep -q 'thread apply all bt' fake-gdb.log
# But the essentials are still there:
tap_check grep -q -x 'argv\[0\]=--nx' fake-gdb.log
tap_check grep -q -x 'argv\[1\]=--core' fake-gdb.log
tap_check grep -q -x 'argv\[2\]=v22_run5.elf' fake-gdb.log
tap_check grep -q -x 'argv\[3\]=/tmp/fake_prog.elf' fake-gdb.log
tap_done "--interactive: stripped argv (no --batch / canned commands)"

# ---- 17e. --interactive + --rom-elf still passes add-symbol-file ----

rm -f fake-gdb.log
"$BINARY" idf coredump --core v22.bin --save-core v22_run6.elf \
	--gdb "$PWD/fake-gdb" --interactive --rom-elf "$PWD/fake-rom.elf" \
	/tmp/fake_prog.elf >out 2>err
tap_check grep -qx "argv\\[4\\]=-ex" fake-gdb.log
tap_check grep -qx "argv\\[5\\]=add-symbol-file $PWD/fake-rom.elf" \
	fake-gdb.log
tap_done "--interactive composes with --rom-elf"

# ---- 18. BIN_V* + <prog>: save-core fails first; gdb is NOT invoked ----

rm -f fake-gdb.log
if "$BINARY" idf coredump --core v2.bin --save-core /tmp/should_not_exist \
	--gdb "$PWD/fake-gdb" /tmp/fake_prog.elf >out 2>err; then
	tap_check false
else
	tap_check grep -q 'ELF synthesis' err
	# fake-gdb must NOT have been called -- log should be absent.
	tap_check test ! -e fake-gdb.log
fi
tap_done "gdb is not spawned when save-core fails (BIN_V*)"

tap_result
