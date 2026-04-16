all: $(PREFIX)/lib/libflasher.a

$(PREFIX)/lib/libflasher.a: $(CURDIR)/CMakeLists.txt
	@echo "Building esp-serial-flasher..."
	@cmake -S $(CURDIR) -B build -Wno-dev \
		$(CMAKE_CROSS_FLAGS) \
		-DCMAKE_C_COMPILER="$(CC)" \
		-DCMAKE_INSTALL_PREFIX=$(PREFIX) \
		-DPORT=USER_DEFINED && \
	cmake --build build -j$(NPROC) && \
	cmake --install build
	@echo "esp-serial-flasher done."

clean:
	rm -rf build

.PHONY: all clean
