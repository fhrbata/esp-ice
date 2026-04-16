PREFIX ?= $(CURDIR)/install
NPROC := $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 2)
STAMP := $(PREFIX)/.stamp

# S and TRIPLE are passed by the parent Makefile.
# CROSS=1 when target triple != build machine triple.
# CC flows through MAKEFLAGS from the parent make invocation --
# we don't touch it here.

ifdef CROSS
ifeq ($(S),linux)
CMAKE_CROSS_FLAGS := -DCMAKE_SYSTEM_NAME=Linux
else ifeq ($(S),win)
CMAKE_CROSS_FLAGS := -DCMAKE_SYSTEM_NAME=Windows
else ifeq ($(S),macos)
CMAKE_CROSS_FLAGS := -DCMAKE_SYSTEM_NAME=Darwin
endif
endif

export PREFIX NPROC S TRIPLE CMAKE_CROSS_FLAGS

all: $(STAMP)

$(STAMP): esp-serial-flasher
	@touch $@

esp-serial-flasher:
	$(MAKE) -C esp-serial-flasher

clean:
	$(MAKE) -C esp-serial-flasher clean
	rm -rf $(PREFIX)

# mrproper wipes the entire install tree (all TRIPLEs) and all
# build artifacts.  `clean` only targets the current PREFIX.
mrproper:
	$(MAKE) -C esp-serial-flasher clean
	rm -rf $(CURDIR)/install

.PHONY: all clean mrproper esp-serial-flasher
