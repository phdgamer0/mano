PREFIX ?= $(HOME)/.local
BUILD_DIR ?= build
CMAKE ?= cmake

CMAKE_ARGS := -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ifneq ($(origin CC),default)
CMAKE_ARGS += -DCMAKE_C_COMPILER=$(CC)
endif

.PHONY: all mano asan install uninstall clean

all: mano

mano:
	$(CMAKE) -S . -B $(BUILD_DIR) $(CMAKE_ARGS) -DMANO_ASAN=OFF
	$(CMAKE) --build $(BUILD_DIR) --parallel

asan:
	$(CMAKE) -S . -B $(BUILD_DIR) $(CMAKE_ARGS) -DMANO_ASAN=ON
	$(CMAKE) --build $(BUILD_DIR) --parallel

install: mano
	$(CMAKE) --install $(BUILD_DIR) --prefix $(PREFIX)

uninstall:
	rm -f $(PREFIX)/bin/mano

clean:
	rm -rf $(BUILD_DIR) mano compile_commands.json src/*.o src/*.d
