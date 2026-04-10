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

### Static build (release)

Requires a C compiler, `make`, `cmake`, `curl`, and `bzip2`. All
libraries (musl, zlib, mbedTLS, curl) are built from source automatically.
The resulting binary is fully static with zero runtime dependencies.

```bash
# Debian/Ubuntu
sudo apt-get install gcc make cmake curl bzip2

make STATIC=1
```

### Windows

Building on Windows requires [MSYS2](https://www.msys2.org/). Native
MSVC builds are not supported.

```bash
# In MSYS2 MINGW64 shell
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake make curl bzip2

make STATIC=1
```

### macOS

```bash
brew install cmake
make STATIC=1
```

### Useful targets

| Target | Description |
|--------|-------------|
| `make` | Development build (system libcurl) |
| `make STATIC=1` | Fully static release build |
| `make deps` | Build vendored deps only |
| `make test` | Run tests |
| `make clean` | Remove build artifacts |
| `make mrproper` | Remove build artifacts and vendored deps |
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
