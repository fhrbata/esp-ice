VERSION := 93dded38d350b8d13a3d5b23fe39f1702afc8ab9
URL     := https://github.com/Dzarda7/esp-serial-flasher/archive/$(VERSION).tar.gz
TARBALL := esp-serial-flasher-$(VERSION).tar.gz
SRC     := esp-serial-flasher-$(VERSION)

PREFIX ?= $(CURDIR)/install
AR     ?= ar

all: $(PREFIX)/lib/libespserialflasher.a

$(PREFIX)/lib/libespserialflasher.a: $(TARBALL)
	@echo "Building esp-serial-flasher..."
	@rm -rf $(SRC)
	@tar xf $(TARBALL)
	@mkdir -p $(PREFIX)/lib $(PREFIX)/include
	@$(CC) -c -I$(SRC)/include -I$(SRC)/private_include \
	    $(SRC)/src/esp_elf_image.c -o $(SRC)/esp_elf_image.o
	@$(CC) -c -I$(SRC)/include -I$(SRC)/private_include \
	    $(SRC)/src/sha256.c -o $(SRC)/sha256.o
	@$(AR) rcs $@ $(SRC)/esp_elf_image.o $(SRC)/sha256.o
	@cp -r $(SRC)/include/. $(PREFIX)/include/
	@rm -rf $(SRC)
	@echo "esp-serial-flasher done."

$(TARBALL):
	curl -fsSL -o $@ $(URL)

clean:
	rm -rf $(SRC)

mrproper: clean
	rm -f $(TARBALL)

.PHONY: all clean mrproper
