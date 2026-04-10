VERSION = 0.1.0
NAME := ice

O ?= build
DIST ?= dist
STAGE ?= stage
CFLAGS ?= -Wall -Werror -std=c99 -pedantic
LDFLAGS ?=

T ?= t/
PFLAGS ?=
T_OUT ?= t_out

# Cross-compilation: make STATIC=1 CROSS_COMPILE=arm-linux-gnueabihf- targz-pkg
ifdef CROSS_COMPILE
CC := $(CROSS_COMPILE)gcc
endif

_CC_PREFIX := $(subst $(lastword $(subst -, ,$(CC))),,$(CC))
WINDRES := $(_CC_PREFIX)windres

BUILD_CFLAGS = $(CFLAGS) $(CFLAGS_APPEND)
BUILD_LDFLAGS = $(LDFLAGS) $(LDFLAGS_APPEND)
BUILD_DEFINES := -DVERSION=\"$(VERSION)\"

COMPILER_VERSION := $(shell $(CC) --version)
CONTEXT := "$(COMPILER_VERSION) $(CFLAGS) $(LDFLAGS)"

DUMPMACHINE := $(shell $(CC) -dumpmachine)

ifneq ($(findstring x86_64,$(DUMPMACHINE)),)
	ARCH := amd64
else ifneq ($(findstring i686,$(DUMPMACHINE)),)
	ARCH := i386
else ifneq ($(findstring aarch64,$(DUMPMACHINE)),)
	ARCH := arm64
else ifneq ($(findstring arm64,$(DUMPMACHINE)),)
	ARCH := arm64
else ifneq ($(findstring arm,$(DUMPMACHINE)),)
	# Check for Hard Float vs Soft Float
	ifneq ($(findstring gnueabihf,$(DUMPMACHINE)),)
		ARCH := armhf
	else
		ARCH := armel
	endif
else ifneq ($(findstring powerpc64le,$(DUMPMACHINE)),)
	ARCH := ppc64el
else ifneq ($(findstring s390x,$(DUMPMACHINE)),)
	ARCH := s390x
else ifneq ($(findstring riscv64,$(DUMPMACHINE)),)
	ARCH := riscv64
endif

ifneq ($(findstring mingw,$(DUMPMACHINE)),)
	S ?= win
else ifneq ($(findstring windows,$(DUMPMACHINE)),)
	S ?= win
else ifneq ($(findstring apple,$(DUMPMACHINE)),)
	S ?= macos
else
	S ?= linux
endif

PKG_NAME := $(NAME)-$(VERSION)-$(S)-$(ARCH)$(PKG_SUFFIX)

SRCS := ice.c \
	cmd/build/build.c \
	color.c \
	error.c \
	options.c \
	sbuf.c \
	svec.c \
	http.c

# STATIC=1: use vendored deps from deps/ (fully self-contained)
# Linux: fully static via musl (zero runtime deps)
# macOS: static deps, dynamic system frameworks
# Windows: fully static via MinGW
# otherwise: use system libcurl (for development)
ifdef STATIC
DEPS_PREFIX := $(CURDIR)/deps/install
DEPS_STAMP := $(CURDIR)/deps/.stamp
BUILD_CFLAGS += -I$(DEPS_PREFIX)/include -DCURL_STATICLIB
LIBS := -L$(DEPS_PREFIX)/lib -L$(DEPS_PREFIX)/lib64 -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -ltfpsacrypto -lz
ifeq ($(S),linux)
CC := $(DEPS_PREFIX)/bin/musl-gcc
LDFLAGS += -static
MUSL := 1
# ppc64le: musl requires 64-bit long double (not IEEE 128-bit)
ifeq ($(ARCH),ppc64el)
BUILD_CFLAGS += -mlong-double-64
endif
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

OBJS := $(patsubst %.c,$(O)/%.o,$(notdir $(SRCS)))
ifeq ($(S), win)
	OBJS += $(O)/manifest.o
endif

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

$(O):
	mkdir -p $@

$(DIST):
	mkdir -p $@

$(O)/context: FORCE | $(O)
	@echo $(CONTEXT) | md5sum | cmp -s - $@ || echo $(CONTEXT) | md5sum > $@

$(O)/%.o: %.c Makefile $(O)/context | $(O)
	$(CC) $(BUILD_DEFINES) $(BUILD_CFLAGS) -MD -MP -o $@ -c $<

$(O)/%.o: platform/posix/%.c Makefile $(O)/context  | $(O)
	$(CC) $(BUILD_DEFINES) $(BUILD_CFLAGS) -MD -MP -o $@ -c $<

$(O)/%.o: platform/win/%.c Makefile $(O)/context | $(O)
	$(CC) $(BUILD_DEFINES) $(BUILD_CFLAGS) -MD -MP -o $@ -c $<

$(O)/%.o: cmd/build/%.c Makefile $(O)/context | $(O)
	$(CC) $(BUILD_DEFINES) $(BUILD_CFLAGS) -MD -MP -o $@ -c $<

ifdef STATIC
$(OBJS): | $(DEPS_STAMP)
$(DEPS_STAMP):
	$(MAKE) -C deps S=$(S) $(if $(MUSL),MUSL=1)
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
	$(MAKE) -C deps S=$(S) $(if $(MUSL),MUSL=1)

clean:
	rm -rf $(O) $(DIST) $(STAGE) $(T_OUT)

mrproper: clean
	$(MAKE) -C deps clean

clang-format:
	clang-format --style=file -i *.[ch] platform/posix/*.[ch] platform/win/*.[ch] cmd/*/*.[ch]

# Lint checks and WarningsAsErrors: see .clang-tidy in this directory.
clang-tidy:
	clang-tidy \
		--extra-arg="--target=$(DUMPMACHINE)" \
		$(SRCS) \
		-- \
		$(BUILD_DEFINES) \
		$(BUILD_CFLAGS)

BINARY_ABS := $(abspath $(BINARY))
test: $(BINARY)
	T_OUT=$(T_OUT) BINARY=$(BINARY_ABS) CC=${CC} S=$(S) prove $(PFLAGS) $(T)


cscope:
	find . \( -name "*.c" -o -name "*.h" \) >cscope.files
	cscope -b

ctags:
	find . \( -name "*.c" -o -name "*.h" \) | ctags -L -

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
	@echo ''
	@echo 'test variables:'
	@echo ' T                - directory with tests using TAP(default: $(T))'
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
