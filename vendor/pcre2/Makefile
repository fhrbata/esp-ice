VERSION := 10.44
URL := https://github.com/PCRE2Project/pcre2/releases/download/pcre2-$(VERSION)/pcre2-$(VERSION).tar.gz
TARBALL := pcre2-$(VERSION).tar.gz
SRC := pcre2-$(VERSION)

all: $(PREFIX)/lib/libpcre2-8.a

$(TARBALL):
	curl -fsSL -o $@ $(URL)

$(PREFIX)/lib/libpcre2-8.a: $(TARBALL)
	@echo "Building pcre2 $(VERSION)..."
	@rm -rf $(SRC)
	@tar xf $(TARBALL)
	@cd $(SRC) && \
		CC="$(CC)" ./configure --prefix=$(PREFIX) $(CONFIGURE_HOST) \
			--disable-shared --enable-static \
			--disable-pcre2-16 --disable-pcre2-32 \
			--disable-jit \
			--disable-pcre2grep-callout \
			--disable-pcre2test-libedit \
			--disable-pcre2test-libreadline && \
		$(MAKE) -j$(NPROC) && \
		$(MAKE) install
	@echo "pcre2 done."

clean:
	rm -rf $(SRC)

.PHONY: all clean
