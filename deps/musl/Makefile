VERSION := 1.2.6
URL := https://musl.libc.org/releases/musl-$(VERSION).tar.gz
TARBALL := musl-$(VERSION).tar.gz
SRC := musl-$(VERSION)

all: $(PREFIX)/bin/musl-gcc

$(TARBALL):
	curl -fsSL -o $@ $(URL)

$(PREFIX)/bin/musl-gcc: $(TARBALL)
	@echo "Building musl $(VERSION)..."
	@tar xf $(TARBALL)
	@cd $(SRC) && \
		./configure --prefix=$(PREFIX) --syslibdir=$(PREFIX)/lib && \
		$(MAKE) -j$(NPROC) && \
		$(MAKE) install
	@echo "musl done."

clean:
	rm -rf $(SRC)

.PHONY: all clean
