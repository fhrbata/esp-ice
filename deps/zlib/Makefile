VERSION := 1.3.2
URL := https://zlib.net/zlib-$(VERSION).tar.gz
TARBALL := zlib-$(VERSION).tar.gz
SRC := zlib-$(VERSION)

# s390x: zlib auto-enables VX vector extensions which require
# flags the default target arch (z196) does not support.  Strip them.

all: $(PREFIX)/lib/libz.a

$(TARBALL):
	curl -fsSL -o $@ $(URL)

$(PREFIX)/lib/libz.a: $(TARBALL)
	@echo "Building zlib $(VERSION)..."
	@tar xf $(TARBALL)
	@cd $(SRC) && \
		CC="$(CC)" ./configure --prefix=$(PREFIX) --static && \
		$(if $(filter s390x,$(ARCH)), \
			sed -i 's/crc32_vx\.o//g; s/-DHAVE_S390X_VX//g' Makefile &&) \
		$(MAKE) -j$(NPROC) && \
		$(MAKE) install
	@echo "zlib done."

clean:
	rm -rf $(SRC)

.PHONY: all clean
