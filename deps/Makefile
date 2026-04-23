PREFIX ?= $(CURDIR)/install
NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
STAMP := $(PREFIX)/.stamp

# S and TRIPLE are passed by the parent Makefile.
# CROSS=1 when target triple != build machine triple.
# CC flows through MAKEFLAGS from the parent make invocation —
# we don't touch it here.

ifdef CROSS
CONFIGURE_HOST := --host=$(TRIPLE)
ifeq ($(S),linux)
CMAKE_CROSS_FLAGS := -DCMAKE_SYSTEM_NAME=Linux
else ifeq ($(S),win)
CMAKE_CROSS_FLAGS := -DCMAKE_SYSTEM_NAME=Windows
else ifeq ($(S),macos)
CMAKE_CROSS_FLAGS := -DCMAKE_SYSTEM_NAME=Darwin
endif
endif

export PREFIX NPROC S TRIPLE CONFIGURE_HOST CMAKE_CROSS_FLAGS

all: $(STAMP)

$(STAMP): curl xz
	@touch $@

curl: mbedtls zlib
	$(MAKE) -C curl

mbedtls:
	$(MAKE) -C mbedtls

zlib:
	$(MAKE) -C zlib

xz:
	$(MAKE) -C xz

clean:
	$(MAKE) -C zlib clean
	$(MAKE) -C mbedtls clean
	$(MAKE) -C curl clean
	$(MAKE) -C xz clean
	rm -rf $(PREFIX) $(STAMP)

# mrproper wipes the entire install tree (all TRIPLEs) and all
# extracted sub-lib sources.  `clean` only targets the current PREFIX.
mrproper:
	$(MAKE) -C zlib clean
	$(MAKE) -C mbedtls clean
	$(MAKE) -C curl clean
	$(MAKE) -C xz clean
	rm -rf $(CURDIR)/install

.PHONY: all clean mrproper zlib mbedtls curl xz
