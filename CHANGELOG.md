## v0.5.0 (2026-04-24)

### ✨ New Features

- **cmd/init**: intercept gen_crt_bundle.py in sitecustomize.py *(Frantisek Hrbata - a2ea6d1)*
- **cmd/idf/crt-bundle**: native x509 certificate bundle generator *(Frantisek Hrbata - e4ae0f5)*
- **cmd/idf/hints**: print HINT lines in yellow *(Frantisek Hrbata - a4ac2ae)*
- **cmd/idf/hints**: match hints.yml rules against a log file *(Frantisek Hrbata - 8aa3a4a)*
- **cmd/idf/kconfgen**: drop-in parity with python esp-idf-kconfig *(Frantisek Hrbata - f3b11b4)*
- **cmd/idf/kconfgen**: sdkconfig.cmake hex formatting matches python *(Frantisek Hrbata - 6629a75)*
- **cmd/idf/kconfgen**: kconfig_menus.json implicit-menuconfig tree shape *(Frantisek Hrbata - 46dc68d)*
- **cmd/idf/kconfgen**: kconfig_menus.json parity -- choice and id shape *(Frantisek Hrbata - 2610dd6)*
- **cmd/idf/kconfgen**: final sdkconfig formatting polish *(Frantisek Hrbata - a7055c9)*
- **cmd/idf/kconfgen**: close output-parity gaps against python *(Frantisek Hrbata - f0635ae)*
- **cmd/idf/kconfgen**: disambiguate choice-vs-config same-name collisions *(Frantisek Hrbata - a3f443a)*
- **cmd/idf/kconfgen**: narrow emit filter + auto-load per-component renames *(Frantisek Hrbata - b1ee7a3)*
- **cmd/idf/kconfgen**: wire compatibility with ESP-IDF build invocations *(Frantisek Hrbata - 7c8595f)*
- **cmd/idf/kconfgen**: json / json_menus output formats *(Frantisek Hrbata - bfe711d)*
- **cmd/idf/kconfgen**: --env / --env-file / --list-separator for build parity *(Frantisek Hrbata - a63eaf5)*
- **cmd/idf/kconfgen**: sdkconfig.rename + deprecated-aliases block *(Frantisek Hrbata - e19acf1)*
- **cmd/idf/kconfgen**: choice-group mutual exclusion + declaration-order emit *(Frantisek Hrbata - c5dbfee)*
- **cmd/idf/kconfgen**: sdkconfig.h and sdkconfig.cmake writers *(Frantisek Hrbata - 62c64ea)*
- **cmd/idf/kconfgen**: sdkconfig load / write *(Frantisek Hrbata - d62d1f9)*
- **cmd/idf/kconfgen**: symbol evaluator + --dump-symbols *(Frantisek Hrbata - 88cdfdd)*
- **cmd/idf/kconfgen**: native Kconfig parser with --dump-ast *(Frantisek Hrbata - b182bd4)*
- **smap**: generic string-keyed hash map *(Frantisek Hrbata - d97bed8)*
- **cmd/size**: porcelain wrapper around ice idf size *(Frantisek Hrbata - 4599726)*
- **cmd/idf/ldgen**: native linker-script generator *(Frantisek Hrbata - 73066d4)*
- **cmake**: honour core.build-always in setup_project *(Frantisek Hrbata - d391ad6)*
- auto-run hints on failed process_run_progress *(Frantisek Hrbata - 6b19df4)*

### 🐛 Bug Fixes

- **cmd/idf/kconfgen**: silence clang-tidy warnings *(Frantisek Hrbata - 8116b43)*
- **cmd/idf/kconfgen**: three parity bugs uncovered by 10-chip hello_world diff *(Frantisek Hrbata - ed0ea7b)*
- **cmd/idf/kconfgen**: match python sdkconfig byte-for-byte *(Frantisek Hrbata - afa95ec)*
- **cmd/idf/kconfgen**: three sdkconfig-parity bugs uncovered by hello_world diff *(Frantisek Hrbata - f13e63a)*
- **cmd/idf/kconfgen**: full hello_world coverage on all supported chips *(Frantisek Hrbata - 3a05011)*
- **cmd/idf/kconfgen**: unbounded menu walks + visibility-gated defaults *(Frantisek Hrbata - da7005a)*
- **cmd/log**: expand color tokens before writing to stdout *(Frantisek Hrbata - 6da649c)*

### 📖 Documentation

- **contributing**: codify platform abstraction rules *(Frantisek Hrbata - e7b158e)*
- consolidate onboarding into `ice docs getting-started` *(Frantisek Hrbata - 180e0e2)*

### 🔧 Code Refactoring

- **platform**: PLATFORM_VENV_PYTHON_REL for venv interpreter path *(Frantisek Hrbata - fde4e8b)*
- **cmd/init**: replace patch_ninja with venv + sitecustomize.py *(Frantisek Hrbata - a8b68c4)*
- **hints**: return hints via svec instead of printing *(Frantisek Hrbata - 96ae214)*
- **platform**: #ifdef-free call sites, UTF-8-safe path shims *(Frantisek Hrbata - 7815000)*
- **cmd/init**: consolidate ninja patchers, add ldgen rule *(Frantisek Hrbata - b706b18)*

### 🗑️ Removals

- **menuconfig**: drop ice menuconfig command *(Frantisek Hrbata - ebf85a4)*


## v0.4.0 (2026-04-21)

### ✨ New Features

- **port**: add USB JTAG Serial support (VID=0x303A PID=0x1001) - Add serial_get_usb_id() to serial.h with Linux sysfs implementation   in platform/posix/serial.c and a no-op stub in platform/win/serial.c. - Detect USB JTAG Serial at port init in esf_port.c by comparing the   VID/PID returned by serial_get_usb_id() against 0x303A/0x1001. - Use the USBJTAGSerialReset bootloader entry sequence (matches esptool)   for USB JTAG Serial devices: assert BOOT before RESET, transition   through (1,1) to avoid glitch, then release RESET. - After any reset of a USB JTAG Serial device, wait up to 3 s for the   port to reappear following USB re-enumeration. - Plain USB-UART bridges (CP2102, CH340, FT232) are unaffected. *(Jaroslav Burian - 6f6b6d0)*
- **image**: accept esptool legacy underscore flags in create *(Frantisek Hrbata - c307244)*
- **progress**: dump captured log on failure with filter callback *(Frantisek Hrbata - 50275f4)*
- **init**: bypass python deps check and component manager *(Frantisek Hrbata - fbf5348)*
- **init**: route tool installation through ice tools install *(Frantisek Hrbata - 86730e9)*
- **progress**: add slow-hint suffix and erase with ANSI [K *(Frantisek Hrbata - 33ecc9d)*
- **repo**: split clone/pull/checkout into porcelain + hidden plumbing *(Frantisek Hrbata - 8f62b7d)*
- **init**: route cmake configure through process_run_progress *(Frantisek Hrbata - d11ed4f)*
- **build**: route cmake through process_run_progress *(Frantisek Hrbata - 032ace1)*
- **progress**: add process_run_progress helper for tee-with-progress *(Frantisek Hrbata - 6f11fce)*
- **help**: surface GETTING STARTED right after DESCRIPTION *(Frantisek Hrbata - d8643c3)*
- **pager**: add --no-pager, core.pager config, availability check *(Frantisek Hrbata - 2432e06)*
- **error**: add hint() helper for unified hint output *(Frantisek Hrbata - ff0e5c0)*
- **completion**: add descriptions to shell completion candidates *(Frantisek Hrbata - 0ac2101)*
- **status**: add ice status subcommand *(Frantisek Hrbata - 981247c)*
- **options**: declare config and env defaults per option *(Frantisek Hrbata - 61221c8)*
- **options**: render multi-positional synopsis naturally *(Frantisek Hrbata - d0ac826)*
- **idf**: make checkout name optional, default to ref *(Frantisek Hrbata - bcee6e5)*
- **idf**: serialise clone/pull/checkout with a reference lock *(Frantisek Hrbata - 8816f13)*
- **config**: add config_load_buf and git-style subsection parsing *(Frantisek Hrbata - 0728de5)*
- **slip**: SLIP (RFC 1055) byte-stuffing framer *(Frantisek Hrbata - 0af73b9)*
- **serial**: portable serial port backend *(Frantisek Hrbata - a488902)*
- **image**: add "ice image merge" subcommand *(Frantisek Hrbata - 8c35a43)*
- **image**: add "ice image info" subcommand *(Frantisek Hrbata - bd1559d)*
- **image**: add "ice image" namespace with elf2image command *(Frantisek Hrbata - 6979cd1)*
- **elf2image**: native ELF to ESP flash image engine *(Frantisek Hrbata - d50ab7a)*
- **vendor**: import Brad Conte SHA-256 *(Frantisek Hrbata - b9c2be9)*
- **elf**: add program-header (segment) reader *(Frantisek Hrbata - 96bfabe)*
- **deps**: vendor xz-utils 5.4.7 for liblzma decoder *(Frantisek Hrbata - 948c3c2)*
- **install**: append `ice` next-step to post-install output *(Frantisek Hrbata - e8e119c)*
- ice log, setup_project dispatcher, .ice/ project layout *(Frantisek Hrbata - dcfdec9)*
- serial port auto-detection for flash and monitor - Add serial_list_ports() / serial_free_port_list() to serial.h with   POSIX (glob ttyUSB*/ttyACM*/cu.*) and Win32 (registry SERIALCOMM)   implementations. The returned array is NULL-terminated so only the   pointer is needed to free it. - ice flash: parse target chip from flasher_args.json   extra_esptool_args.chip, scan available ports with esp-serial-flasher   when no port is configured, and abort with a clear mismatch error   when an explicit port is given but the connected chip differs from   the firmware target. - ice monitor: probe available ports with esp-serial-flasher to find   the first ESP device, reset it back to normal run mode, then open   the port for monitoring. - Move esf_chip_name() and esf_find_esp_port() to port/esf_port.{h,c}   to eliminate duplication between flash and monitor. *(Jaroslav Burian - 98f08cc)*
- implement native esp-serial-flasher port and rewrite ice flash - Add platform-independent time_now_ms() and delay_ms(uint32_t) to   platform.h, implemented via CLOCK_MONOTONIC/nanosleep on POSIX and   GetTickCount64/Sleep on Windows. - Add port/esf_port.{h,c}: an esp_loader_port_ops_t vtable backed   by esp-ice's serial.h API, with HardReset and UnixTightReset sequencing. - Rewrite cmd/flash/flash.c to use esp-serial-flasher directly: parses   flasher_args.json, connects to the chip, flashes each image with an   in-place progress bar, and prints chip name and negotiated baud rate. - Add -p/--port and -b/--baud CLI options to ice flash. - Register port/esf_port.c in Makefile LIB_SRCS. *(Jaroslav Burian - c82859b)*
- add cross-platform lockfile primitive *(Frantisek Hrbata - 26b51f9)*
- declarative option parsing with OPT_SUBCOMMAND and --ice-complete *(Frantisek Hrbata - 75f7a06)*
- auto-inject tool PATH and env vars on startup *(Frantisek Hrbata - a32322b)*
- add `ice tools` namespace, remove top-level install and set-target *(Frantisek Hrbata - 3c951e7)*
- add `ice target` namespace, hide `set-target` *(Frantisek Hrbata - 73c6f77)*
- add `ice idf` namespace for managing ESP-IDF source *(Frantisek Hrbata - 1a6fb0b)*
- add `ice install` command for ESP-IDF tools *(Frantisek Hrbata - 0479024)*
- add `ice monitor` serial port monitor command *(Frantisek Hrbata - ee6a25b)*
- add esp-serial-flasher as vendored dependency *(Frantisek Hrbata - ebe1127)*
- add tar archive extraction (.tar / .tar.gz / .tar.xz) *(Frantisek Hrbata - bfa4dcf)*

### 🐛 Bug Fixes

- **esf**: drive DTR/RTS atomically to avoid bootloader-entry glitch *(Frantisek Hrbata - e5ccba3)*
- **esf**: release BOOT/RESET in UnixTightReset final step *(Frantisek Hrbata - 1c6222e)*
- **binary**: add missing RTC SLOW memory range for esp32s3 *(Jaroslav Burian - 8ea143e)*
- **completion**: update set-target references to target command *(Jaroslav Burian - e5b76d9)*
- **ci**: clang-format, clang-analyzer warnings, tidy config, and test link *(Jaroslav Burian - deab584)*
- **toolenv**: select tool version from active tools.json *(Frantisek Hrbata - ed1cdd7)*
- **init**: handle script-form esptool in elf2image ninja patch *(Frantisek Hrbata - 4ed49c3)*
- **init**: drop CMakeCache.txt requirement when wiping build dir *(Frantisek Hrbata - 9008d32)*
- **cli**: extend namespace-bare-shows-manual to leaves with required positionals *(Frantisek Hrbata - cd0f065)*
- **cli**: show manual when a namespace is invoked with no subcommand *(Frantisek Hrbata - e1ac5af)*
- **term**: use @} for literal-} escape, not }} *(Frantisek Hrbata - becb6da)*
- **fs**: suppress signal-handler lint for platform-macro'd unlink *(Frantisek Hrbata - 7b18fa3)*
- **cmake**: load_profile gates on .iceconfig and overrides IDF_PATH *(Frantisek Hrbata - e323846)*
- **cmake**: compare strncmp result explicitly in complete_profile_names *(Frantisek Hrbata - b17b878)*
- **color**: expand @x{} tokens after %s substitution, restore outer on close *(Frantisek Hrbata - 047794c)*
- **config**: emit git-style subsections for three-part keys *(Frantisek Hrbata - 57c12e6)*
- **options**: stop --ice-complete leaking past a positional argument *(Frantisek Hrbata - f7d7acd)*
- **idf**: silence rmdir warnings and keep reference attached to master *(Frantisek Hrbata - 5166ae3)*
- **idf**: keep submodule objects across checkouts *(Frantisek Hrbata - 5c5775a)*
- **test**: pass LINK_LIBS to test compile lines for libice deps *(Frantisek Hrbata - eadfd25)*
- **ci**: install cmake in Windows MSYS2 environment *(Frantisek Hrbata - e6d4af0)*
- **ci**: scope _POSIX_C_SOURCE to Linux only *(Frantisek Hrbata - 40effb0)*
- **ci**: export CPATH for Homebrew headers on macOS *(Frantisek Hrbata - 716aaa9)*
- **lint**: fix remaining implicit-widening in create and merge *(Frantisek Hrbata - 9b029d8)*
- **lint**: fix clang-tidy warnings in create, merge, serial *(Frantisek Hrbata - 688f913)*
- **platform**: make platform.h self-contained (include stdio, stdarg) *(Frantisek Hrbata - 8b7e43f)*

### 📖 Documentation

- add getting-started guide to root help *(Frantisek Hrbata - 15acd11)*

### 🔧 Code Refactoring

- **cmd/flash**: split porcelain/plumbing via hidden __flash *(Frantisek Hrbata - 855c423)*
- **cmd/target**: nest flash under its own subdirectory *(Frantisek Hrbata - 8868317)*
- **chip**: centralize chip identity into chip.h/chip.c *(Jaroslav Burian - c0a0174)*
- **cmd**: match directory layout to ice command hierarchy *(Frantisek Hrbata - a63ef34)*
- **cmake**: centralize IDF_COMPONENT_MANAGER=0 in load_profile *(Frantisek Hrbata - 5fddcb7)*
- **tools**: merge cmd/install/install.c into cmd/tools/tools.c *(Frantisek Hrbata - 5d69d57)*
- **options**: replace "[<...>]" argh convention with OPT_POSITIONAL_OPT *(Frantisek Hrbata - baab96c)*
- **cmake**: make ice init the sole owner of cmake configure state *(Frantisek Hrbata - deb9cc2)*
- **config**: promote profile keys into uniform project.* namespace *(Frantisek Hrbata - a05c8ee)*
- **cmake**: load_profile reads .iceconfig fresh, no global mirror *(Frantisek Hrbata - d1b73c1)*
- **cmake**: make build_dir/generator/defines file-scope to cmake.c *(Frantisek Hrbata - 05acbb2)*
- **idf**: move monitor/configdep/ldgen/partition-table/size under ice idf *(Frantisek Hrbata - 63849f7)*
- **cmake**: route build/flash/clean/menuconfig through profile *(Frantisek Hrbata - 105ee8a)*
- **init**: make ice init the only project bind, with profile support *(Frantisek Hrbata - 60fa677)*
- **options**: drop OPT_END_COMPLETE for OPT_POSITIONAL + extra_complete *(Frantisek Hrbata - 2953bb7)*
- **repo**: rename ice idf namespace to ice repo *(Frantisek Hrbata - e1db857)*
- **options**: remove OPT_SUBCOMMAND machinery *(Frantisek Hrbata - a83ca65)*
- **options**: dispatch through ice_dispatch and the descriptor tree *(Frantisek Hrbata - b4a55e3)*
- **options**: declare a cmd_desc for every command *(Frantisek Hrbata - 69a0a5c)*
- **options**: parse_options takes a struct cmd_desc *(Frantisek Hrbata - 1dd8ce0)*
- **options**: add cmd_desc and ice_dispatch scaffolding *(Frantisek Hrbata - 0290b24)*
- **idf**: collapse to reference + named checkouts *(Frantisek Hrbata - a7aaa6b)*
- **idf**: rework clone/switch for fast parallel submodule handling *(Frantisek Hrbata - 693c6df)*
- **install**: single-line per-tool progress with erase_line *(Frantisek Hrbata - bef81b9)*
- **serial**: remove #ifdef from serial.h, add full Windows backend *(Frantisek Hrbata - 855cd7b)*
- **image**: rename "ice image elf2image" → "ice image create" *(Frantisek Hrbata - baa699d)*
- **binary**: extract image-format knowledge from elf2image *(Frantisek Hrbata - 6eefb2f)*
- unify parse_options API and polish completion *(Frantisek Hrbata - 45f0125)*
- remove redundant #include <string.h> from new files *(Frantisek Hrbata - 0b8d8e2)*
- use ~/.ice as home directory, add ice_home() and HOME_ENV *(Frantisek Hrbata - 00566ef)*

### 🗑️ Removals

- **target**: drop ice target info *(Frantisek Hrbata - e27a8f8)*
- **cmake**: drop ice cmake wrapper *(Frantisek Hrbata - 0355fb1)*


## v0.3.1 (2026-04-15)

### 🐛 Bug Fixes

- **install**: point default ICE_REPO at fhrbata/esp-ice *(Frantisek Hrbata - 1597b4f)*


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
