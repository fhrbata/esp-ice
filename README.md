# esp-ice

*Ice ice baby. Too cold -- slice like a ninja, cut like a razor blade.*

## What is ice?

`ice` is a single-binary frontend for ESP-IDF projects.  It replaces
`idf.py`, `export.sh`, and the per-project Python virtual environment
with one self-contained executable that fetches its own ESP-IDF source
tree and toolchains on demand.

Highlights:

- Single self-contained static binary.  Python is still used at build
  time for a handful of IDF tools that haven't been reimplemented in
  C yet; ice manages a minimal Python environment internally so you
  don't source `export.sh` or run `pip install` yourself.
- Built-in commands for building, flashing, monitoring, configuring,
  and analysing firmware size -- one tool, one help system.
- Per-project profiles in `.ice/config` so the same checkout can build
  for multiple chips or sdkconfigs without conflict.
- Managed ESP-IDF reference under `~/.ice/` with cheap named checkouts
  that share git objects across versions.

> **Experimental PoC** -- this project is a proof of concept and is
> not intended for production use.

## Install

### Prebuilt binaries

Prebuilt static binaries for Linux, macOS, and Windows are published
with each release.  The one-liners below download the matching archive,
extract `ice` to a user-writable directory, and print a completion
setup hint tailored to your shell.

#### Linux / macOS

```sh
curl -fsSL https://raw.githubusercontent.com/fhrbata/esp-ice/main/install.sh | sh
```

Default install location is `$HOME/.local/bin/ice`.  Override with
`ICE_INSTALL_DIR`, or pin a specific version with `ICE_VERSION`:

```sh
ICE_VERSION=0.2.0 ICE_INSTALL_DIR=/usr/local/bin curl -fsSL https://raw.githubusercontent.com/fhrbata/esp-ice/main/install.sh | sh
```

#### Windows

```powershell
irm https://raw.githubusercontent.com/fhrbata/esp-ice/main/install.ps1 | iex
```

Default install location is `%LOCALAPPDATA%\Programs\ice\bin\ice.exe`
and is added to your user `PATH` automatically.  Override with
`$env:ICE_INSTALL_DIR` or `$env:ICE_VERSION`.

Once installed, run `ice docs getting-started` for the guided
walkthrough -- from fresh install to a flashed `hello_world`.

### From source

#### Development build

Requires a C compiler, `make`, and the development headers for
libcurl, zlib, and liblzma.

```bash
# Debian/Ubuntu
sudo apt-get install gcc make libcurl4-openssl-dev zlib1g-dev liblzma-dev

# Fedora
sudo dnf install gcc make libcurl-devel zlib-devel xz-devel

make
```

This links dynamically against the system libcurl, zlib, and liblzma
for fast iteration.  Override `CC` to pick a different compiler
(e.g. `make CC=clang`).

#### Static build (release)

All vendored libraries (zlib, mbedTLS, curl, libyaml, xz) are built
from source automatically.  The resulting binary is fully static with
zero runtime dependencies.

```bash
# macOS
brew install cmake
make STATIC=1

# Windows (MSYS2 MINGW64 shell)
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake make curl bzip2
make STATIC=1

# Linux — requires a musl toolchain (see Cross-compilation below)
eval "$(make -f Makefile.toolchain linux-amd64)"
make STATIC=1 CC=x86_64-linux-musl-gcc
```

On Linux, `STATIC=1` requires a musl-based compiler.  glibc static
linking is not truly portable (NSS uses `dlopen` at runtime), so the
build will error if `CC` does not target musl.

#### Cross-compilation

Linux and Windows release binaries are cross-compiled from a Linux
host.  macOS binaries are built natively (cross-compiling for macOS
from Linux would require the Apple SDK).  `Makefile.toolchain`
manages the prebuilt cross-compilation toolchains — it
downloads them on first use and prints `export PATH=…` on stdout.
Wrapping the call in `eval "$(…)"` executes that output in the
current shell, adding the toolchain's `bin/` directory to `PATH` so
the cross-compiler is available for subsequent commands:

```bash
# List available toolchains and their CC values
make -f Makefile.toolchain help

# Fetch a single toolchain and build for Linux ARM64
eval "$(make -f Makefile.toolchain linux-arm64)"
make STATIC=1 CC=aarch64-linux-musl-gcc targz-pkg

# Fetch all toolchains at once and build for multiple targets
eval "$(make -f Makefile.toolchain all)"
make STATIC=1 CC=x86_64-linux-musl-gcc targz-pkg
make STATIC=1 CC=aarch64-linux-musl-gcc targz-pkg
make STATIC=1 CC=x86_64-w64-mingw32-gcc zip-pkg
```

`CC` selects the cross-compiler, which in turn determines the target
platform, architecture, output directory (`build/<triple>`), and deps
install path (`deps/install/<triple>`).  Build artifacts are scoped by
the compiler triple, so multiple targets can coexist in the same
checkout without colliding.

#### Build variables

| Variable | Description |
|----------|-------------|
| `CC` | Compiler to use (default: `cc`). Determines the target triple, output directory, and deps install path. |
| `STATIC=1` | Build vendored deps from source and link statically. |
| `O` | Output directory (default: `build/<triple>`). |

Run `make help` for the full list of variables, targets, and their
current values.  Run `make -f Makefile.toolchain help` for the
catalogue of available cross-compilation toolchains.

#### Useful targets

| Target | Description |
|--------|-------------|
| `make` | Development build (system libcurl) |
| `make STATIC=1` | Fully static release build |
| `make deps` | Build vendored deps only |
| `make test` | Run tests |
| `make clean` | Remove build artifacts for the current triple |
| `make mrproper` | Remove all build artifacts and vendored deps |
| `make help` | Show all build variables and targets |

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for development setup,
repository layout, the platform-abstraction rules, the command-wiring
conventions, and walkthroughs for adding new commands.

## License

This project is licensed under the Apache License 2.0. See the [LICENSE](LICENSE) file for details.

Copyright 2026 Espressif Systems (Shanghai) CO LTD
