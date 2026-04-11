VERSION := 0.2.5
URL := https://github.com/yaml/libyaml/releases/download/$(VERSION)/yaml-$(VERSION).tar.gz
TARBALL := yaml-$(VERSION).tar.gz
SRC := yaml-$(VERSION)

all: $(PREFIX)/lib/libyaml.a

$(TARBALL):
	curl -fsSL -o $@ $(URL)

$(PREFIX)/lib/libyaml.a: $(TARBALL)
	@echo "Building libyaml $(VERSION)..."
	@rm -rf $(SRC)
	@tar xf $(TARBALL)
	@cd $(SRC) && \
		CC="$(CC)" ./configure --prefix=$(PREFIX) $(CONFIGURE_HOST) \
			--disable-shared --enable-static && \
		$(MAKE) -j$(NPROC) && \
		$(MAKE) install
	@echo "libyaml done."

clean:
	rm -rf $(SRC)

.PHONY: all clean
