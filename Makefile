VERSION = 0.1.0
NAME := ice

O ?= build/$(TRIPLE)
DIST ?= dist
STAGE ?= stage
CFLAGS ?= -Wall -Werror -std=c99 -pedantic
LDFLAGS ?=

# Tests live per-command under cmd/<name>/t/, with shared TAP helpers
# (tap.sh, tap.h) and any cross-cutting tests in the top-level t/.
# Override T on the command line to run a subset, e.g.
#   make test T=cmd/completion/t
#   make test T=cmd/completion/t/0001-completion.t
T ?= $(wildcard cmd/*/t) t
PFLAGS ?=
T_OUT ?= t_out

# Default parallel build on the host running make.  Try nproc (Linux
# and MSYS2), fall back to sysctl (macOS), finally 1 if neither works.
# Override with `make JOBS=1` to debug a parallel-build failure, or
# pass `-j N` on the command line -- the last -j wins, so user flags
# always take precedence over MAKEFLAGS.
JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1)
MAKEFLAGS += -j$(JOBS)

# Target triple.  Prefer the CC name's prefix (e.g. aarch64-w64-mingw32-clang
# → aarch64-w64-mingw32) over $(CC) -dumpmachine, because llvm-mingw's clang
# reports its triple as "aarch64-w64-windows-gnu" which autotools config.sub
# rejects.  Unprefixed compilers (gcc/clang/cc) fall back to -dumpmachine.
CC_PREFIX := $(patsubst %-,%,$(subst $(lastword $(subst -, ,$(CC))),,$(CC)))
ifneq ($(CC_PREFIX),)
TRIPLE := $(CC_PREFIX)
CROSS_COMPILE := $(CC_PREFIX)-
else
TRIPLE := $(shell $(CC) -dumpmachine 2>/dev/null)
CROSS_COMPILE :=
endif
WINDRES := $(CROSS_COMPILE)windres

BUILD_CFLAGS = $(CFLAGS) $(CFLAGS_APPEND)
BUILD_LDFLAGS = $(LDFLAGS) $(LDFLAGS_APPEND)
BUILD_DEFINES := -DVERSION=\"$(VERSION)\"

# Project root on the include search path so sources under cmd/<name>/
# and platform/<os>/ can #include "ice.h" directly instead of reaching
# up with "../../ice.h".  Quoted-form includes still find sibling
# headers (e.g. cmd/ldgen/lf.h) in the source file's own directory.
BUILD_CFLAGS += -I$(CURDIR)

COMPILER_VERSION := $(shell $(CC) --version)
CONTEXT := "$(COMPILER_VERSION) $(CFLAGS) $(LDFLAGS)"

ifneq ($(findstring x86_64,$(TRIPLE)),)
	ARCH := amd64
else ifneq ($(findstring i686,$(TRIPLE)),)
	ARCH := i386
else ifneq ($(findstring aarch64,$(TRIPLE)),)
	ARCH := arm64
else ifneq ($(findstring arm64,$(TRIPLE)),)
	ARCH := arm64
else ifneq ($(findstring arm,$(TRIPLE)),)
	# Check for Hard Float vs Soft Float
	ifneq ($(findstring gnueabihf,$(TRIPLE))$(findstring musleabihf,$(TRIPLE)),)
		ARCH := armhf
	else
		ARCH := armel
	endif
else ifneq ($(findstring powerpc64le,$(TRIPLE)),)
	ARCH := ppc64el
else ifneq ($(findstring s390x,$(TRIPLE)),)
	ARCH := s390x
else ifneq ($(findstring riscv64,$(TRIPLE)),)
	ARCH := riscv64
endif

ifneq ($(findstring mingw,$(TRIPLE)),)
	S ?= win
else ifneq ($(findstring windows,$(TRIPLE)),)
	S ?= win
else ifneq ($(findstring apple,$(TRIPLE)),)
	S ?= macos
else
	S ?= linux
endif

# Cross-compilation: target triple != build machine triple.
ifneq ($(TRIPLE),$(shell cc -dumpmachine 2>/dev/null))
CROSS := 1
endif

PKG_NAME := $(NAME)-$(VERSION)-$(S)-$(ARCH)$(PKG_SUFFIX)

SRCS := ice.c \
	ar.c \
	cmake.c \
	cmakecache.c \
	cmd/build/build.c \
	cmd/clean/clean.c \
	cmd/cmake/cmake.c \
	cmd/completion/completion.c \
	cmd/config/config.c \
	cmd/configdep/configdep.c \
	cmd/flash/flash.c \
	cmd/fullclean/fullclean.c \
	cmd/help/help.c \
	cmd/ldgen/ldgen.c \
	cmd/ldgen/lf.c \
	cmd/menuconfig/menuconfig.c \
	cmd/reconfigure/reconfigure.c \
	cmd/set-target/set-target.c \
	cmd/size/chip.c \
	cmd/size/size.c \
	config.c \
	json.c \
	map.c \
	term.c \
	elf.c \
	error.c \
	help.c \
	pager.c \
	options.c \
	sbuf.c \
	svec.c \
	http.c

# STATIC=1: use vendored deps from deps/ (fully self-contained).
# On Linux this requires a musl CC (enforced below); glibc -static is
# a lie because NSS dlopen()s the target system's libc at runtime, so
# the binary isn't actually portable.  Fetch a musl toolchain via
# Makefile.toolchain.  Windows uses MinGW, macOS uses system clang.
ifdef STATIC
DEPS_PREFIX := $(CURDIR)/deps/install/$(TRIPLE)
DEPS_STAMP := $(DEPS_PREFIX)/.stamp
BUILD_CFLAGS += -I$(DEPS_PREFIX)/include -DCURL_STATICLIB
LIBS := -L$(DEPS_PREFIX)/lib -L$(DEPS_PREFIX)/lib64 -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -ltfpsacrypto -lz
ifeq ($(S),linux)
ifeq ($(findstring musl,$(TRIPLE)),)
$(error STATIC=1 on Linux requires a musl toolchain (got CC='$(CC)', triple='$(TRIPLE)'). Fetch one: eval "$$(make -f Makefile.toolchain linux-<arch>)")
endif
LDFLAGS += -static
endif
ifeq ($(S),macos)
LIBS += -framework CoreFoundation -framework SystemConfiguration
endif
ifeq ($(S),win)
LDFLAGS += -static
endif
else
LIBS := -lcurl
endif

ifeq ($(S), win)

SRCS += platform/win/io.c platform/win/process.c platform/win/wconv.c platform/win/wmain.c
CFLAGS += -municode
LDFLAGS += -municode
LIBS += -lws2_32 -lbcrypt -lwinhttp -liphlpapi
BINARY := $(O)/$(NAME).exe

else

SRCS += platform/posix/posix_io.c platform/posix/posix_process.c
BINARY := $(O)/$(NAME)

endif

OBJS := $(patsubst %.c,$(O)/%.o,$(SRCS))
ifeq ($(S), win)
	OBJS += $(O)/manifest.o
endif

OBJDIRS := $(sort $(patsubst %/,%,$(dir $(OBJS))))

all: $(BINARY)

define MANIFEST_XML
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity
    type="win32"
    name="$(NAME)"
    version="$(VERSION).0"
    processorArchitecture="*"
  />
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings xmlns:ws2="http://schemas.microsoft.com/SMI/2016/WindowsSettings">
      <ws2:longPathAware>true</ws2:longPathAware>
    </windowsSettings>
  </application>
</assembly>
endef

$(O)/manifest.xml: Makefile | $(O)
	$(file >$@,$(MANIFEST_XML))

$(O)/manifest.rc: $(O)/manifest.xml
	$(file >$@,1 24 "$<")

$(O)/manifest.o: $(O)/manifest.rc $(O)/context
	$(WINDRES) --input=$< --output=$@

$(OBJDIRS):
	mkdir -p $@

$(DIST):
	mkdir -p $@

$(O)/context: FORCE | $(O)
	@echo $(CONTEXT) | md5sum | cmp -s - $@ || echo $(CONTEXT) | md5sum > $@

$(O)/%.o: %.c Makefile $(O)/context | $(OBJDIRS)
	$(CC) $(BUILD_DEFINES) $(BUILD_CFLAGS) -MD -MP -o $@ -c $<

ifdef STATIC
$(OBJS): | $(DEPS_STAMP)
$(DEPS_STAMP):
	$(MAKE) -C deps PREFIX=$(DEPS_PREFIX) S=$(S) TRIPLE=$(TRIPLE) $(if $(CROSS),CROSS=1)
endif

$(BINARY): $(OBJS) | $(O)
	$(CC) -o $@ $^ $(BUILD_LDFLAGS) $(LIBS)

.PHONY: clean mrproper deps \
	targz-pkg \
	tarxz-pkg \
	zip-pkg \
	clang-format \
	clang-tidy \
	test \
	cscope \
	ctags \
	tags \
	help

FORCE:

targz-pkg: $(DIST)/$(PKG_NAME).tar.gz
tarxz-pkg: $(DIST)/$(PKG_NAME).tar.xz
zip-pkg: $(DIST)/$(PKG_NAME).zip

$(STAGE): $(BINARY)
	@rm -rf $@
	@install -d $@/$(NAME)-$(VERSION)/bin
	@install $(BINARY) $(STAGE)/$(NAME)-$(VERSION)/bin
	@touch $@

$(DIST)/$(PKG_NAME).tar.gz: $(STAGE) | $(DIST)
	cd $(STAGE) && tar -cvzf $(abspath $@) $(NAME)-$(VERSION)

$(DIST)/$(PKG_NAME).tar.xz: $(STAGE) | $(DIST)
	cd $(STAGE) && tar -cvJf $(abspath $@) $(NAME)-$(VERSION)

$(DIST)/$(PKG_NAME).zip: $(STAGE) | $(DIST)
	cd $(STAGE) && zip -r $(abspath $@) $(NAME)-$(VERSION)

deps:
	$(MAKE) -C deps PREFIX=$(CURDIR)/deps/install/$(TRIPLE) S=$(S) TRIPLE=$(TRIPLE) $(if $(CROSS),CROSS=1)

clean:
	rm -rf $(O) $(DIST) $(STAGE) $(T_OUT)

mrproper: clean
	rm -rf $(CURDIR)/build
	$(MAKE) -C deps mrproper

# Routed through pre-commit so everyone uses the version pinned in
# .pre-commit-config.yaml.  Raw clang-format output drifts between
# releases even with the same .clang-format config.
clang-format:
	pre-commit run clang-format --all-files

# Lint checks and WarningsAsErrors: see .clang-tidy in this directory.
clang-tidy:
	clang-tidy \
		--extra-arg="--target=$(TRIPLE)" \
		$(SRCS) \
		-- \
		$(BUILD_DEFINES) \
		$(BUILD_CFLAGS)

BINARY_ABS := $(abspath $(BINARY))
test: $(BINARY)
	T_OUT=$(T_OUT) BINARY=$(BINARY_ABS) CC=${CC} S=$(S) prove $(PFLAGS) $(T)


TAG_DIRS := cmd platform

cscope:
	{ find . -maxdepth 1 -name "*.[ch]"; find $(TAG_DIRS) -name "*.[ch]"; } >cscope.files
	cscope -b

ctags:
	{ find . -maxdepth 1 -name "*.[ch]"; find $(TAG_DIRS) -name "*.[ch]"; } | ctags -L -

tags: cscope ctags

help:
	@echo 'build variables:'
	@echo ' CC               - compiler to use e.g. gcc-i686-linux-gnu or x86_64-w64-mingw32-gcc (default: $(CC))'
	@echo ' O                - output build directory (default: $(O))'
	@echo ' S                - operating system win/posix/macos (default: autodetected)'
	@echo ' CFLAGS           - compiler options (default: $(CFLAGS))'
	@echo ' LDFLAGS          - linker options (default: $(LDFLAGS))'
	@echo ' CFLAGS_APPEND    - additional compiler options to append after CFLAGS'
	@echo ' LDFLAGS_APPEND   - additional linker options to append after LDFLAGS'
	@echo ' JOBS             - parallel build jobs; `-j N` on the command line wins (default: $(JOBS))'
	@echo ''
	@echo 'test variables:'
	@echo ' T                - test path(s) passed to prove; override to run a subset,'
	@echo '                    e.g. T=cmd/completion/t or T=cmd/completion/t/0001.t'
	@echo '                    (default: $(T))'
	@echo ' T_OUT            - directory with test artifacts (default: $(T_OUT))'
	@echo ' PFLAGS           - prove options for running tests through TAP harness  (default: $(PFLAGS))'
	@echo ''
	@echo 'test targets:'
	@echo ' test             - run all tests  (default: $(PFLAGS))'
	@echo ''
	@echo 'distribution variables:'
	@echo ' DIST             - output directory for distribution files (default: $(DIST))'
	@echo ' STAGE            - stagging directory for distribution preparation (default: $(STAGE))'
	@echo ''
	@echo 'distribution targets:'
	@echo ' targz-pkg        - create tar.gz archive for distribution'
	@echo ' tarxz-pkg        - create tar.xz archive for distribution'
	@echo ' zip-pkg          - create zip archive for distribution'
	@echo ''
	@echo 'lint targets:'
	@echo ' clang-format     - run clang formatter'
	@echo ' clang-tidy       - run clang tidy'
	@echo ''
	@echo 'dependency targets:'
	@echo ' deps             - build vendored deps (zlib, mbedTLS, curl)'
	@echo ''
	@echo 'misc targets:'
	@echo ' clean            - remove: $(O) $(DIST) $(STAGE) $(T_OUT)'
	@echo ' mrproper         - clean + remove vendored deps'
	@echo ' cscope           - generate cscope tags'
	@echo ' ctags            - generate ctags'
	@echo ' tags             - alias for cscope and tags'

-include $(O)/*.d
