VERSION = 0.3.1
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

# Run prove with one worker per core by default; override PFLAGS to
# re-serialize (PFLAGS=-j1) or to add verbosity (PFLAGS="-j4 -v").
PFLAGS ?= -j$(JOBS)
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
BUILD_DEFINES = -DVERSION=\"$(VERSION)\" -DICE_PLATFORM_OS=\"$(S)\" -DICE_PLATFORM_ARCH=\"$(ARCH)\"

# Project root on the include search path so sources under cmd/<name>/
# and platform/<os>/ can #include "ice.h" directly instead of reaching
# up with "../../ice.h".  Quoted-form includes still find sibling
# headers (e.g. cmd/ldgen/lf.h) in the source file's own directory.
BUILD_CFLAGS += -I$(CURDIR)

# SANITIZE=1: enable AddressSanitizer for the whole build (libice,
# ice, and the test binaries via SAN_FLAGS below).  Incompatible with
# STATIC=1 because libasan is dynamically linked; release builds
# shouldn't ship it anyway.  Append `-fsanitize=undefined` to SAN_FLAGS
# manually if you also have libubsan available -- not all distros
# ship it by default (Fedora packages it separately, for example).
ifdef SANITIZE
ifdef STATIC
$(error SANITIZE=1 is incompatible with STATIC=1; sanitizers need a dynamic libasan runtime)
endif
SAN_FLAGS := -fsanitize=address -fno-omit-frame-pointer
BUILD_CFLAGS  += $(SAN_FLAGS)
BUILD_LDFLAGS += $(SAN_FLAGS)
endif

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

# LIB_SRCS go into libice.a -- everything reusable except the program
# entry point.  Tests link against the archive directly so they don't
# need to know which .c files implement which transitive dependency.
# Static-archive linking also drops dead code from the final binary
# (today: ar.c, elf.c, http.c have no live callers from main()).
LIB_SRCS := \
	ar.c \
	cmake.c \
	cmakecache.c \
	cmd/build/build.c \
	cmd/clean/clean.c \
	cmd/completion/completion.c \
	cmd/config/config.c \
	cmd/configdep/configdep.c \
	cmd/flash/flash.c \
	cmd/help/help.c \
	cmd/image/image.c \
	cmd/image/create.c \
	cmd/image/info.c \
	cmd/image/merge.c \
	cmd/idf/idf.c \
	cmd/init/init.c \
	cmd/ldgen/ldgen.c \
	cmd/ldgen/lf.c \
	cmd/monitor/monitor.c \
	cmd/menuconfig/menuconfig.c \
	cmd/partition-table/partition-table.c \
	cmd/repo/repo.c \
	cmd/target/target.c \
	cmd/tools/tools.c \
	cmd/size/chip.c \
	cmd/size/size.c \
	cmd/status/status.c \
	config.c \
	csv.c \
	fs.c \
	ice.c \
	json.c \
	map.c \
	md5.c \
	term.c \
	elf.c \
	error.c \
	help.c \
	pager.c \
	options.c \
	partition_table.c \
	binary.c \
	elf2image.c \
	progress.c \
	sbuf.c \
	slip.c \
	svec.c \
	http.c \
	gzip.c \
	xz.c \
	reader.c \
	tar.c \
	toolenv.c \
	vendor/sha256/sha256.c

# MAIN_SRCS provide the program entry point.  Excluded from libice.a
# so that unit tests (and any future external libice consumer) can
# supply their own main().
MAIN_SRCS := main.c

SRCS := $(MAIN_SRCS) $(LIB_SRCS)

# STATIC=1: use vendored deps from deps/ (fully self-contained).
# On Linux this requires a musl CC (enforced below); glibc -static is
# a lie because NSS dlopen()s the target system's libc at runtime, so
# the binary isn't actually portable.  Fetch a musl toolchain via
# Makefile.toolchain.  Windows uses MinGW, macOS uses system clang.
ifdef STATIC
DEPS_PREFIX := $(CURDIR)/deps/install/$(TRIPLE)
DEPS_STAMP := $(DEPS_PREFIX)/.stamp
BUILD_CFLAGS += -I$(DEPS_PREFIX)/include -DCURL_STATICLIB
LIBS := -L$(DEPS_PREFIX)/lib -L$(DEPS_PREFIX)/lib64 -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -ltfpsacrypto -lz -llzma
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
# Dev mode: link against system libraries.  -lz isn't strictly
# required (libcurl pulls it in transitively for HTTP gzip), but
# being explicit removes a fragile assumption and matches what we
# actually call directly.  liblzma is needed for .tar.xz extraction;
# the system package is xz-devel (Fedora) / liblzma-dev (Debian).
LIBS := -lcurl -lz -llzma
endif

# Vendor libraries (always built from source, unlike deps which are
# only needed for static/release builds).
VENDOR_PREFIX := $(CURDIR)/vendor/install/$(TRIPLE)
VENDOR_STAMP := $(VENDOR_PREFIX)/.stamp
BUILD_CFLAGS += -I$(VENDOR_PREFIX)/include
LIBS += -L$(VENDOR_PREFIX)/lib -lflasher

ifeq ($(S), win)

# wmain.c provides the Windows wide-char entry point that calls into
# main() from ice.c, so it lives with MAIN_SRCS.  io / process / wconv
# are reusable platform glue and go into libice.a.
LIB_SRCS  += platform/win/console.c platform/win/io.c platform/win/process.c platform/win/serial.c platform/win/wconv.c
MAIN_SRCS += platform/win/wmain.c
SRCS      := $(MAIN_SRCS) $(LIB_SRCS)
CFLAGS += -municode
LDFLAGS += -municode
LIBS += -lws2_32 -lbcrypt -lwinhttp -liphlpapi
BINARY := $(O)/$(NAME).exe

else

LIB_SRCS += platform/posix/console.c platform/posix/io.c platform/posix/process.c platform/posix/serial.c
SRCS     := $(MAIN_SRCS) $(LIB_SRCS)
BINARY := $(O)/$(NAME)
# glibc hides POSIX symbols (readlink, popen, …) under -std=c99
# unless _POSIX_C_SOURCE is defined.  macOS exposes everything by
# default; defining _POSIX_C_SOURCE there would *restrict* visibility
# and hide Darwin extensions like B115200 in <termios.h>.
ifeq ($(S),linux)
CFLAGS_APPEND += -D_POSIX_C_SOURCE=200112L
endif

endif

LIB_OBJS  := $(patsubst %.c,$(O)/%.o,$(LIB_SRCS))
MAIN_OBJS := $(patsubst %.c,$(O)/%.o,$(MAIN_SRCS))
ifeq ($(S), win)
	# manifest.o is per-binary metadata, not part of the library.
	MAIN_OBJS += $(O)/manifest.o
endif
OBJS := $(MAIN_OBJS) $(LIB_OBJS)

LIBICE     := $(O)/libice.a
LIBICE_ABS := $(abspath $(LIBICE))

# Default to the cross-compile prefix when present; users can still
# override on the command line (e.g. `make AR=clang-ar`).
AR := $(CROSS_COMPILE)ar

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

$(OBJS): | $(VENDOR_STAMP)
$(VENDOR_STAMP):
	$(MAKE) -C vendor PREFIX=$(VENDOR_PREFIX) S=$(S) TRIPLE=$(TRIPLE) $(if $(CROSS),CROSS=1)

$(LIBICE): $(LIB_OBJS)
	$(AR) rcs $@ $^

# Link MAIN_OBJS first so ld picks up only the libice.a members the
# binary actually references; unused modules (e.g. http.o today) drop
# out of the binary along with anything they would have pulled from
# system libs.
$(BINARY): $(MAIN_OBJS) $(LIBICE) | $(O)
	$(CC) -o $@ $(MAIN_OBJS) $(LIBICE) $(BUILD_LDFLAGS) $(LIBS)

.PHONY: clean mrproper deps vendor \
	libice \
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

# Convenience target: build only the static library.
libice: $(LIBICE)

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

vendor:
	$(MAKE) -C vendor PREFIX=$(CURDIR)/vendor/install/$(TRIPLE) S=$(S) TRIPLE=$(TRIPLE) $(if $(CROSS),CROSS=1)

clean:
	rm -rf $(O) $(DIST) $(STAGE) $(T_OUT)

mrproper: clean
	rm -rf $(CURDIR)/build
	$(MAKE) -C deps mrproper
	$(MAKE) -C vendor mrproper

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

# ASan defaults to running LeakSanitizer at process exit; for a CLI
# tool that legitimately keeps state in static storage until exit
# (alias-expansion tokens, the global config, ...) this would flag
# benign "leaks" on every run.  Disable it by default and rely on
# the still-active checks (use-after-free, overflow, double-free,
# stack/heap buffer over/underflow) -- a strict leak audit is a
# follow-up that needs explicit atexit cleanup of those globals.
SAN_TEST_ENV := LSAN_OPTIONS=detect_leaks=0

test: $(BINARY) $(LIBICE)
	T_OUT=$(T_OUT) BINARY=$(BINARY_ABS) LIBICE=$(LIBICE_ABS) \
		LINK_LIBS="$(LIBS)" \
		SAN_FLAGS="$(SAN_FLAGS)" $(if $(SANITIZE),$(SAN_TEST_ENV)) \
		CC=${CC} S=$(S) prove $(PFLAGS) $(T)


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
	@echo ' SANITIZE         - SANITIZE=1 enables AddressSanitizer (incompatible with STATIC=1)'
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
	@echo ' libice           - build the static library (linked by ice and tests)'
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
	@echo ' deps             - build external deps (zlib, mbedTLS, curl, libyaml, xz)'
	@echo ' vendor           - build vendor libs (esp-serial-flasher)'
	@echo ''
	@echo 'misc targets:'
	@echo ' clean            - remove: $(O) $(DIST) $(STAGE) $(T_OUT)'
	@echo ' mrproper         - clean + remove vendored deps'
	@echo ' cscope           - generate cscope tags'
	@echo ' ctags            - generate ctags'
	@echo ' tags             - alias for cscope and tags'

-include $(O)/*.d
