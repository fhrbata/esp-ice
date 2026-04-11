PREFIX ?= $(CURDIR)/install
NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
STAMP := $(CURDIR)/.stamp

# HOST_CC is the user's compiler, passed explicitly from the parent
# Makefile.  For MUSL builds, musl is built with HOST_CC first and then
# everything else is compiled with musl-gcc (which wraps HOST_CC).  For
# non-MUSL builds (Windows cross, macOS) HOST_CC is used directly.
HOST_CC ?= $(CC)

# Derive the target triple from HOST_CC.  Prefer the compiler-name
# prefix (e.g. aarch64-w64-mingw32-clang → aarch64-w64-mingw32) because
# llvm-mingw's clang reports -dumpmachine as "aarch64-w64-windows-gnu",
# which autotools' config.sub rejects.  Fall back to -dumpmachine for
# unprefixed compilers (gcc, clang).
HOST_CC_PREFIX := $(patsubst %-,%,$(subst $(lastword $(subst -, ,$(HOST_CC))),,$(HOST_CC)))
ifneq ($(HOST_CC_PREFIX),)
HOST_TRIPLE := $(HOST_CC_PREFIX)
else
HOST_TRIPLE := $(shell $(HOST_CC) -dumpmachine 2>/dev/null)
endif
BUILD_TRIPLE := $(shell cc -dumpmachine 2>/dev/null)
ifneq ($(HOST_TRIPLE),$(BUILD_TRIPLE))
CROSS := 1
CONFIGURE_HOST := --host=$(HOST_TRIPLE)
# Static linking for configure tests: on i386 musl-gcc, the hidden
# __stack_chk_fail_local symbol isn't available for dynamic linking,
# so configure link tests would fail without -static.
CROSS_STATIC := -static
# CMake cross-compilation: set target system name from S.
ifeq ($(S),linux)
CMAKE_CROSS_FLAGS := -DCMAKE_SYSTEM_NAME=Linux
else ifeq ($(S),win)
CMAKE_CROSS_FLAGS := -DCMAKE_SYSTEM_NAME=Windows
else ifeq ($(S),macos)
CMAKE_CROSS_FLAGS := -DCMAKE_SYSTEM_NAME=Darwin
endif
endif

# ppc64le: Debian Trixie GCC defaults to IEEE 128-bit long double,
# which musl does not support.  Force 64-bit long double everywhere.
ifneq ($(findstring powerpc64le,$(HOST_TRIPLE)),)
export CFLAGS += -mlong-double-64
endif

# i386: musl's cross-compiled libc.a doesn't provide __stack_chk_fail_local.
# Disable stack protector to avoid generating references to it.
ifdef CROSS
ifneq ($(findstring i686,$(HOST_TRIPLE)),)
export CFLAGS += -fno-stack-protector
endif
endif

# MUSL builds: after musl is built, everything else uses musl-gcc.
# Override beats the CC command-line variable inherited from the parent
# make via MAKEFLAGS.
ifdef MUSL
MUSL_DEP := musl
override CC := $(PREFIX)/bin/musl-gcc
endif

export PREFIX NPROC S HOST_TRIPLE CONFIGURE_HOST CMAKE_CROSS_FLAGS CROSS_STATIC

all: $(STAMP)

$(STAMP): curl libyaml
	@touch $@

# Explicitly pass CC on each sub-make command line so that the override
# above (and the non-MUSL HOST_CC case) wins over any CC that leaks down
# through MAKEFLAGS from the user-level make invocation.
curl: mbedtls zlib
	$(MAKE) -C curl CC="$(CC)"

mbedtls: $(MUSL_DEP)
	$(MAKE) -C mbedtls CC="$(CC)"

zlib: $(MUSL_DEP)
	$(MAKE) -C zlib CC="$(CC)"

libyaml: $(MUSL_DEP)
	$(MAKE) -C libyaml CC="$(CC)"

musl:
	$(MAKE) -C musl CC="$(HOST_CC)"

clean:
	$(MAKE) -C zlib clean
	$(MAKE) -C mbedtls clean
	$(MAKE) -C curl clean
	$(MAKE) -C libyaml clean
	$(MAKE) -C musl clean
	rm -rf $(PREFIX) $(STAMP)

.PHONY: all clean zlib mbedtls curl libyaml musl
