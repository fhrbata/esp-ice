VERSION := 8.18.0
URL := https://curl.se/download/curl-$(VERSION).tar.gz
TARBALL := curl-$(VERSION).tar.gz
SRC := curl-$(VERSION)

# mbedTLS transitive deps needed for curl's configure link test.
# Windows additionally needs bcrypt (entropy) and ws2_32 (sockets).
MBEDTLS_LIBS := -lmbedx509 -lmbedcrypto -ltfpsacrypto
ifeq ($(S),win)
MBEDTLS_LIBS += -lbcrypt -lws2_32
endif

all: $(PREFIX)/lib/libcurl.a

$(TARBALL):
	curl -fsSL -o $@ $(URL)

$(PREFIX)/lib/libcurl.a: $(TARBALL)
	@echo "Building curl $(VERSION)..."
	@tar xf $(TARBALL)
	@cd $(SRC) && \
		CC="$(CC)" \
		CFLAGS="-I$(PREFIX)/include" \
		LDFLAGS="-L$(PREFIX)/lib -L$(PREFIX)/lib64" \
		LIBS="$(MBEDTLS_LIBS)" \
		./configure --prefix=$(PREFIX) $(CONFIGURE_HOST) \
			--disable-shared --enable-static \
			--with-mbedtls=$(PREFIX) --with-zlib=$(PREFIX) \
			--without-openssl --without-gnutls \
			--without-wolfssl --without-bearssl \
			--without-nghttp2 --without-nghttp3 \
			--without-ngtcp2 --without-libidn2 \
			--without-libpsl --without-brotli \
			--without-zstd --without-c-ares \
			--disable-ldap --disable-dict \
			--disable-ftp --disable-imap \
			--disable-pop3 --disable-smtp \
			--disable-telnet --disable-tftp \
			--disable-rtsp --disable-manual && \
		$(MAKE) -j$(NPROC) && \
		$(MAKE) install
	@echo "curl done."

clean:
	rm -rf $(SRC)

.PHONY: all clean
