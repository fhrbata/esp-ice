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
		./configure --prefix=$(PREFIX) --disable-shared && \
		$(MAKE) -j$(NPROC) && \
		$(MAKE) install
	@# Default to static linking -- the musl dynamic linker is not
	@# installed on the host, so configure scripts that try to run
	@# compiled test programs would fail without this.
	@sed -i 's/-dynamic-linker [^ ]*//' $(PREFIX)/lib/musl-gcc.specs
	@echo "musl done."

clean:
	rm -rf $(SRC)

.PHONY: all clean
