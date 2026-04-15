## v0.3.0 (2026-04-15)

### ✨ New Features

- **install**: add curl|sh and iwr|iex release installer scripts *(Frantisek Hrbata - c113c85)*
- **completion**: add PowerShell shell support *(Frantisek Hrbata - 4b27aae)*


## v0.2.0 (2026-04-15)

### ✨ New Features

- **platform**: add rename_w for atomic-replace semantics on Windows *(Frantisek Hrbata - 84a7bd4)*
- **platform**: add is_directory, unlink_w, rmdir_w *(Frantisek Hrbata - 4b7ccf7)*
- **platform**: add dir_foreach primitive *(Frantisek Hrbata - dd16d84)*
- **build**: replace gen_esp32part.py invocations with ice partition-table *(Jaroslav Burian - c5e8ef8)*
- write_file_atomic() in fs.c; migrate ninja-patch and csv_save *(Frantisek Hrbata - d0140e0)*
- add csv.c/csv.h -- RFC 4180 reader and writer *(Frantisek Hrbata - ad03008)*
- add fs.c with portable mkdirp / rmtree helpers *(Frantisek Hrbata - ee5fe72)*
- add partition-table subcommand *(Jaroslav Burian - 8d3a95b)*
- add get_executable_path() for POSIX and Windows *(Jaroslav Burian - b07d403)*
- shell completion for bash, zsh, and fish *(Frantisek Hrbata - d185918)*
- add 'ice fullclean' and 'ice set-target' subcommands *(Frantisek Hrbata - 3d72edc)*
- manual pages for every subcommand *(Frantisek Hrbata - 3400d18)*
- git-style short usage for bare 'ice' and 'ice help' *(Frantisek Hrbata - f20cc65)*
- colorize -h usage and list aliases in 'ice --help' *(Frantisek Hrbata - cb5afbc)*
- add man-style --help for every subcommand *(Frantisek Hrbata - 4a6756d)*
- add 'ice config' subcommand *(Frantisek Hrbata - a5f0833)*
- expand alias.<name> config entries *(Frantisek Hrbata - 57db274)*
- add in-house JSON DOM reader/writer *(Frantisek Hrbata - 6083f9f)*
- add git-style cascading config store *(Frantisek Hrbata - fcdfb80)*
- add named color lookup to @[]{} token syntax *(Frantisek Hrbata - b159193)*
- add complete color defines to term.h *(Frantisek Hrbata - 3fb76bf)*
- add table output format and auto target detection *(Frantisek Hrbata - 58f892a)*
- rename color module to term, extend SGR support *(Frantisek Hrbata - 02c6140)*
- add size command with linker map parser and memory map builder *(Frantisek Hrbata - 7f86d19)*
- add configdep command and ar/elf parsers *(Frantisek Hrbata - ebd4f55)*
- implement flag parsing in linker fragment parser *(Frantisek Hrbata - 24ad291)*
- add alloc.h with ALLOC_GROW, improve ldgen docs *(Frantisek Hrbata - fe4044f)*
- add ldgen command with linker fragment (.lf) parser *(Frantisek Hrbata - e91be60)*
- add 'all' target to Makefile.toolchain *(Frantisek Hrbata - cd03055)*
- scope build and deps install by TRIPLE *(Frantisek Hrbata - fe2455b)*
- replace BearSSL with mbedTLS 4, add musl, restructure deps *(Frantisek Hrbata - 1eb1748)*
- replace OpenSSL with BearSSL for TLS *(Frantisek Hrbata - 463fde8)*
- build musl from source for truly clean static binaries *(Frantisek Hrbata - 420c9b7)*
- add Makefile.deps for building static deps from source *(Frantisek Hrbata - 0ef2845)*
- initial commit *(Frantisek Hrbata - 6c4f52a)*

### 🐛 Bug Fixes

- **partition-table**: drop stray commas between H_PARA blocks in manual *(Frantisek Hrbata - db069f9)*
- **partition-table**: expose option table for shell completion *(Frantisek Hrbata - 5a5ab6b)*
- **cmake**: warn when configure fails with CLI -D entries in play *(Frantisek Hrbata - ba0a11a)*
- **size**: restore pointer-to-pointer dereference in cmp_entry_used *(Frantisek Hrbata - 0b93d18)*
- **cmake**: always remove CMakeCache.txt on configure failure *(Jaroslav Burian - 0069d8e)*
- **ci**: fix Windows builds — add bcrypt/ws2_32 for mbedTLS, diffutils *(Frantisek Hrbata - 49145d6)*
- **ci**: build natively on amd64/arm64, Docker+QEMU for the rest *(Frantisek Hrbata - 0bc3a91)*
- **ci**: use matrix.qemu field for QEMU-emulated builds *(Frantisek Hrbata - 0ee3469)*
- **ci**: add bzip2 for mbedTLS tarball, simplify QEMU setup *(Frantisek Hrbata - b88fc16)*
- **ci**: also strip -DHAVE_S390X_VX from zlib build on s390x *(Frantisek Hrbata - 168e24e)*
- **ci**: strip zlib CRC32 VX objects on s390x instead of adding flags *(Frantisek Hrbata - 514ea12)*
- **ci**: inject s390x zlib flags after configure instead of before *(Frantisek Hrbata - 6d3c46d)*
- **ci**: fix ppc64el and s390x builds, add missing arch detection *(Frantisek Hrbata - a591a95)*
- **ci**: use Debian instead of Alpine for Docker builds *(Frantisek Hrbata - 97ae305)*
- **ci**: skip musl build on Alpine (already musl-based) *(Frantisek Hrbata - b2f477f)*
- **ci**: use Alpine for Docker, add Windows link libs *(Frantisek Hrbata - 65675ee)*
- **ci**: create deps prefix dirs before copy, static musl-gcc for configure *(Frantisek Hrbata - a249665)*
- **ci**: remove c-ares-static (not available on Alpine) *(Frantisek Hrbata - 48e8357)*
- **ci**: stub nghttp3_version for Alpine static linking *(Frantisek Hrbata - d3b876f)*
- **ci**: try nghttp3-dev and ngtcp2-dev for Alpine *(Frantisek Hrbata - a93506a)*
- **ci**: install nghttp3-dev from Alpine edge for HTTP/3 symbols *(Frantisek Hrbata - 8e15154)*
- **ci**: add zstd-static, strip ngtcp2 for Alpine *(Frantisek Hrbata - 4a7762d)*
- **ci**: strip nghttp3 from Alpine static link (package unavailable) *(Frantisek Hrbata - e0e6786)*
- **ci**: add libnghttp3-static for Alpine, zip for Windows *(Frantisek Hrbata - 5e70b5c)*
- **ci**: hardcode Alpine static deps, add CURL_STATICLIB for Windows *(Frantisek Hrbata - dd9bc1b)*
- **ci**: use pkg-config --libs --static on all platforms *(Frantisek Hrbata - 9a773e9)*
- **ci**: separate LIBS from LDFLAGS, add nghttp3-static *(Frantisek Hrbata - e5aae09)*
- **ci**: rename posix process.c to avoid collision, remove c-ares-static *(Frantisek Hrbata - dd15fa9)*
- **ci**: rename posix io.c to avoid object name collision, add missing static deps *(Frantisek Hrbata - 23f8788)*
- **ci**: add static curl transitive deps, disable fail-fast *(Frantisek Hrbata - 94e8d6a)*
- address clang-tidy warnings *(Jaroslav Burian - 80cd0eb)*
- emit -h/--help in shell completion *(Frantisek Hrbata - 1a1ee38)*
- ar reader skips entire BSD name field, not just name length *(Frantisek Hrbata - 5c5c9e8)*
- silence clang-tidy errors surfaced by newer upstream checks *(Frantisek Hrbata - 4238b78)*
- reject STATIC=1 on Linux without a musl CC *(Frantisek Hrbata - 9f2df5a)*
- force bash in Makefile.toolchain for pipefail *(Frantisek Hrbata - f463f27)*
- keep .downloaded/.unpacked stamps, don't pollute eval stdout *(Frantisek Hrbata - 308b067)*
- fetch musl.cc toolchains via GitHub mirror *(Frantisek Hrbata - 5a984a2)*
- derive cross HOST_TRIPLE from CC prefix, not -dumpmachine *(Frantisek Hrbata - 5354389)*
- include inherited CFLAGS in curl configure *(Frantisek Hrbata - 1c49bc4)*
- disable stack protector for i386 cross-compilation with musl *(Frantisek Hrbata - 9dca82e)*
- pass -static to curl configure for cross-compilation *(Frantisek Hrbata - fb2f12b)*
- build only libz.a to avoid i386 test program link failure *(Frantisek Hrbata - 30ad9e5)*
- use GitHub mirror for zlib download *(Frantisek Hrbata - 047c8bb)*
- add iphlpapi to Windows link libs for if_nametoindex *(Frantisek Hrbata - 97aa4d7)*
- pass S from main Makefile to deps instead of re-detecting *(Frantisek Hrbata - afbbc6f)*
- disable mbedTLS inline assembly for cross-arch compatibility *(Frantisek Hrbata - a1e0eee)*
- use --disable-crcvx instead of sed hack for s390x zlib *(Frantisek Hrbata - 99e581b)*
- build musl with dynamic linker for configure compatibility *(Frantisek Hrbata - bd49838)*
- force static linking in musl-gcc via self_spec *(Frantisek Hrbata - d9ae030)*
- resolve build failures on all platforms *(Frantisek Hrbata - 4020353)*
- strip dynamic linker from musl-gcc specs *(Frantisek Hrbata - cf7dedb)*
- remove stray out.txt *(Frantisek Hrbata - 962234e)*
- place libraries after objects in link command *(Frantisek Hrbata - c36601c)*
- suppress false positive clang-tidy checks across CI platforms *(Frantisek Hrbata - 68d2cf0)*
- resolve CI pipeline failures across all platforms *(Frantisek Hrbata - 49f2e86)*

### 📖 Documentation

- **partition-table**: add manual page; drop ../../ in includes *(Frantisek Hrbata - ef791f1)*
- correct SANITIZE help text *(Frantisek Hrbata - e716f0d)*
- onboarding guide for adding a new subcommand *(Frantisek Hrbata - f857671)*
- document lf.c lexer and parser internals *(Frantisek Hrbata - 029baed)*
- clarify that macOS builds are native, not cross-compiled *(Frantisek Hrbata - 14a831d)*
- add CC usage examples to cross-compilation section *(Frantisek Hrbata - 5e6a9d9)*
- document cross-compilation, CC, and Makefile.toolchain *(Frantisek Hrbata - c1d336c)*
- add build instructions and experimental notice to README *(Frantisek Hrbata - b3b2274)*
- add wink to tagline *(Frantisek Hrbata - e4c4a00)*

### 🔧 Code Refactoring

- **partition-table**: tighten size, subtype and --flash-size handling *(Frantisek Hrbata - 20ed322)*
- **size**: drop dead nr_ranges > 0 guard *(Frantisek Hrbata - e2a03da)*
- **partition-table**: migrate pt_parse_csv onto csv.c *(Frantisek Hrbata - ff56138)*
- **partition-table**: align dir and file name with other commands *(Frantisek Hrbata - e94b070)*
- **platform**: drop posix_ prefix on posix/{io,process}.c *(Frantisek Hrbata - b7c78e2)*
- **platform**: merge *_exe.c into *_process.c; switch Windows to UTF-8 *(Frantisek Hrbata - a359788)*
- **platform**: rename get_executable_path to process_exe *(Frantisek Hrbata - 6504a6e)*
- **platform**: fold process.h into platform.h *(Frantisek Hrbata - 50fd248)*
- split main() out of ice.c into its own file *(Frantisek Hrbata - d19a407)*
- add project root to include path *(Frantisek Hrbata - 8d2aa53)*
- unify --help rendering, wrap every prose section *(Frantisek Hrbata - ba4c1f6)*
- unify 'ice', 'ice help', and 'ice --help' *(Frantisek Hrbata - 8c9fe4b)*
- split cmake commands, add generic 'ice cmake <target>' *(Frantisek Hrbata - 0ded440)*
- parse CMakeCache.txt once into a struct *(Frantisek Hrbata - b81af86)*
- replace build command with generic cmake target infrastructure *(Frantisek Hrbata - 1ea8935)*
- move color_text into term module as generic API *(Frantisek Hrbata - f55e8b3)*
- drop COLOR_* defines from term.h *(Frantisek Hrbata - fa0a887)*
- rename Windows toolchains to llvm-mingw-{msvcrt,ucrt} *(Frantisek Hrbata - 27853bc)*
- collapse Makefile.toolchain to single curl|tar stage *(Frantisek Hrbata - 3f20154)*
- explicit per-target url/arch/dir + msvcrt/ucrt llvm-mingw *(Frantisek Hrbata - 21d1e55)*
- collapse per-target blocks with pattern rules *(Frantisek Hrbata - e937562)*
- drop windows-* symlink indirection in Makefile.toolchain *(Frantisek Hrbata - 4aacb08)*
- Makefile.toolchain — explicit per-target rules *(Frantisek Hrbata - ad523a7)*
- export S and ARCH from deps orchestrator *(Frantisek Hrbata - eb7ed79)*
- unify architecture naming to Debian convention *(Frantisek Hrbata - d86d7ff)*
