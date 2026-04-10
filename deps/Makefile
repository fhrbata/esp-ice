PREFIX ?= $(CURDIR)/install
NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
STAMP := $(CURDIR)/.stamp
ARCH := $(shell uname -m)
DUMPMACHINE := $(shell $(CC) -dumpmachine 2>/dev/null)

ifneq ($(findstring mingw,$(DUMPMACHINE)),)
S := win
else ifneq ($(findstring apple,$(DUMPMACHINE)),)
S := macos
else
S := linux
endif

# ppc64le: Debian Trixie GCC defaults to IEEE 128-bit long double,
# which musl does not support.  Force 64-bit long double everywhere.
ifeq ($(ARCH),ppc64le)
export CFLAGS += -mlong-double-64
endif

# MUSL=1: build musl first, use musl-gcc for all other deps
HOST_CC := $(CC)
ifdef MUSL
MUSL_DEP := musl
CC := $(PREFIX)/bin/musl-gcc
endif

export PREFIX CC NPROC S ARCH

all: $(STAMP)

$(STAMP): curl libyaml
	@touch $@

curl: mbedtls zlib
	$(MAKE) -C curl

mbedtls: $(MUSL_DEP)
	$(MAKE) -C mbedtls

zlib: $(MUSL_DEP)
	$(MAKE) -C zlib

libyaml: $(MUSL_DEP)
	$(MAKE) -C libyaml

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
