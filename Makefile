# Thin convenience wrapper around CMake. CMakePresets.json is the real build
# definition; run `cmake --list-presets` to see every configuration.
PRESET ?= gcc-release

.PHONY: all build test clean help

all: build

build:
	cmake --preset $(PRESET)
	cmake --build --preset $(PRESET)

test: build
	ctest --preset $(PRESET)

clean:
	rm -rf build

help:
	@echo "make [PRESET=<name>]   configure and build (default: $(PRESET))"
	@echo "make test              build, then run the test suite"
	@echo "make clean             remove the build tree"
	@echo ""
	@echo "Available presets:"
	@cmake --list-presets
