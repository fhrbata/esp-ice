# One cmake tree per target triple: a single shared tree keeps the
# first configure's compiler in its cache, so a later native build
# after a cross build (or the other way round) installs a library
# for the wrong architecture.
BUILD_DIR := build/$(TRIPLE)

all: $(PREFIX)/lib/libflasher.a

$(PREFIX)/lib/libflasher.a: $(CURDIR)/CMakeLists.txt
	@echo "Building esp-serial-flasher..."
	@cmake -S $(CURDIR) -B $(BUILD_DIR) -Wno-dev \
		$(CMAKE_CROSS_FLAGS) \
		-DCMAKE_C_COMPILER="$(CC)" \
		-DCMAKE_INSTALL_PREFIX=$(PREFIX) \
		-DPORT=USER_DEFINED && \
	cmake --build $(BUILD_DIR) -j$(NPROC) && \
	cmake --install $(BUILD_DIR)
	@echo "esp-serial-flasher done."

clean:
	rm -rf build

.PHONY: all clean
