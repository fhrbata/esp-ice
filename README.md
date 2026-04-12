# esp-ice

*Ice ice baby. Too cold -- slice like a ninja, cut like a razor blade.*

> **Experimental PoC** -- this project is a proof of concept and is not
> intended for production use.

## Building

### Development build

Requires a C compiler, `make`, and libcurl development headers.

```bash
# Debian/Ubuntu
sudo apt-get install gcc make libcurl4-openssl-dev

# Fedora
sudo dnf install gcc make libcurl-devel

make
```

This links dynamically against the system libcurl for fast iteration.
Override `CC` to pick a different compiler (e.g. `make CC=clang`).

### Static build (release)

All vendored libraries (zlib, mbedTLS, curl, libyaml) are built from
source automatically.  The resulting binary is fully static with zero
runtime dependencies.

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

### Cross-compilation

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

### Build variables

| Variable | Description |
|----------|-------------|
| `CC` | Compiler to use (default: `cc`). Determines the target triple, output directory, and deps install path. |
| `STATIC=1` | Build vendored deps from source and link statically. |
| `O` | Output directory (default: `build/<triple>`). |

Run `make help` for the full list of variables, targets, and their
current values.  Run `make -f Makefile.toolchain help` for the
catalogue of available cross-compilation toolchains.

### Useful targets

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

Contributions to **esp-ice** are welcome! Please follow these guidelines:

### Development Setup

Before contributing, set up your development environment:

1. Clone the repository:
   ```bash
   git clone https://github.com/espressif/esp-ice.git
   cd esp-ice
   ```

2. Install pre-commit hooks:
   ```bash
   pip install pre-commit
   pre-commit install -t pre-commit -t commit-msg
   ```

   The pre-commit hooks will automatically run before each commit to check code formatting and commit message standards.

### Code Standards

- Follow the existing C code style (enforced by `clang-format`)
- Ensure all commits follow the [Conventional Commits](https://www.conventionalcommits.org/) standard
- Run the test suite before submitting pull requests

### Submitting Changes

1. Create a new branch for your changes
2. Make your changes and ensure pre-commit checks pass
3. Submit a pull request with a clear description of the changes

For more details, see the pre-commit configuration in `.pre-commit-config.yaml`.

## License

This project is licensed under the Apache License 2.0. See the [LICENSE](LICENSE) file for details.

Copyright 2026 Espressif Systems (Shanghai) CO LTD
