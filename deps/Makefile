PREFIX ?= $(CURDIR)/install
NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
STAMP := $(CURDIR)/.stamp

# MUSL=1: build musl first, use musl-gcc for all other deps.
# For cross-compilation, HOST_CC is the cross-compiler (used to build musl);
# after musl is built, CC becomes musl-gcc (which wraps the cross-compiler).
HOST_CC := $(if $(CROSS_COMPILE),$(CROSS_COMPILE)gcc,$(CC))

ifdef CROSS_COMPILE
HOST_TRIPLE := $(patsubst %-,%,$(CROSS_COMPILE))
CONFIGURE_HOST := --host=$(HOST_TRIPLE)
CMAKE_CROSS_FLAGS := -DCMAKE_SYSTEM_NAME=Linux
endif

# ppc64le: Debian Trixie GCC defaults to IEEE 128-bit long double,
# which musl does not support.  Force 64-bit long double everywhere.
ifneq ($(findstring powerpc64le,$(shell $(HOST_CC) -dumpmachine 2>/dev/null)),)
export CFLAGS += -mlong-double-64
endif

ifdef MUSL
MUSL_DEP := musl
CC := $(PREFIX)/bin/musl-gcc
endif

export PREFIX CC NPROC S HOST_TRIPLE CONFIGURE_HOST CMAKE_CROSS_FLAGS

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
