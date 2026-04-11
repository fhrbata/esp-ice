VERSION := 4.1.0
URL := https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$(VERSION)/mbedtls-$(VERSION).tar.bz2
TARBALL := mbedtls-$(VERSION).tar.bz2
SRC := mbedtls-$(VERSION)

MBEDTLS_CONFIG := $(CURDIR)/mbedtls_config.h
CRYPTO_CONFIG := $(CURDIR)/crypto_config.h

all: $(PREFIX)/lib/libmbedtls.a

$(TARBALL):
	curl -fsSL -o $@ $(URL)

$(PREFIX)/lib/libmbedtls.a: $(TARBALL) $(MBEDTLS_CONFIG) $(CRYPTO_CONFIG)
	@echo "Building mbedTLS $(VERSION) (minimal config)..."
	@rm -rf $(SRC)
	@tar xf $(TARBALL)
	@cd $(SRC) && \
		cmake -B build \
			$(CMAKE_CROSS_FLAGS) \
			-DCMAKE_C_COMPILER="$(CC)" \
			-DCMAKE_INSTALL_PREFIX=$(PREFIX) \
			-DMBEDTLS_CONFIG_FILE=$(MBEDTLS_CONFIG) \
			-DTF_PSA_CRYPTO_CONFIG_FILE=$(CRYPTO_CONFIG) \
			-DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
			-DUSE_STATIC_MBEDTLS_LIBRARY=ON \
			-DENABLE_TESTING=OFF \
			-DENABLE_PROGRAMS=OFF \
			-DMBEDTLS_FATAL_WARNINGS=OFF && \
		cmake --build build -j$(NPROC) && \
		cmake --install build
	@echo "mbedTLS done."

clean:
	rm -rf $(SRC)

.PHONY: all clean
