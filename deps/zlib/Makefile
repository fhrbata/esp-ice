VERSION := 1.3.2
URL := https://github.com/madler/zlib/releases/download/v$(VERSION)/zlib-$(VERSION).tar.gz
TARBALL := zlib-$(VERSION).tar.gz
SRC := zlib-$(VERSION)

all: $(PREFIX)/lib/libz.a

$(TARBALL):
	curl -fsSL -o $@ $(URL)

$(PREFIX)/lib/libz.a: $(TARBALL)
	@echo "Building zlib $(VERSION)..."
	@rm -rf $(SRC)
	@tar xf $(TARBALL)
	@cd $(SRC) && \
		$(if $(CROSS),CHOST=$(TRIPLE)) \
		CC="$(CC)" ./configure --prefix=$(PREFIX) --static \
			--disable-crcvx && \
		$(MAKE) -j$(NPROC) libz.a && \
		$(MAKE) install
	@echo "zlib done."

clean:
	rm -rf $(SRC)

.PHONY: all clean
