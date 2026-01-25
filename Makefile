# ============================================================================
# Mars Allocator Makefile
# COMP2221 Systems Programming - Summative Assignment
# ============================================================================

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -Werror -O2 -fPIC -std=c11
LDFLAGS = -shared
DEBUG_FLAGS = -g -DDEBUG -fsanitize=address -fsanitize=undefined
INCLUDES = -I.

# Library and executable names
LIB_NAME = liballocator.so
EXEC_NAME = runme

# Source files
LIB_SOURCES = allocator.c
LIB_HEADERS = allocator.h
LIB_OBJECTS = $(LIB_SOURCES:.c=.o)

EXEC_SOURCES = runme.c
EXEC_OBJECTS = $(EXEC_SOURCES:.c=.o)

# Test parameters
TEST_SEED = 42
TEST_SIZE = 8192
TEST_STORM = 0
TEST_STRESS = 100

# Colors for output
COLOR_GREEN = \033[0;32m
COLOR_BLUE = \033[0;34m
COLOR_YELLOW = \033[0;33m
COLOR_RED = \033[0;31m
COLOR_RESET = \033[0m

# ============================================================================
# Main Targets (Required by Specification)
# ============================================================================

# Default target: build everything
.PHONY: all
all: clean $(LIB_NAME) $(EXEC_NAME)
	@echo "$(COLOR_GREEN)════════════════════════════════════════$(COLOR_RESET)"
	@echo "$(COLOR_GREEN)Build completed successfully!$(COLOR_RESET)"
	@echo "$(COLOR_GREEN)════════════════════════════════════════$(COLOR_RESET)"
	@echo "Library: $(LIB_NAME)"
	@echo "Executable: $(EXEC_NAME)"
	@echo ""
	@echo "Run 'make test' to execute test suite"
	@echo "Run 'make help' for more options"

# Build the shared library
$(LIB_NAME): $(LIB_OBJECTS)
	@echo "$(COLOR_BLUE)[LINKING]$(COLOR_RESET) Creating shared library $@"
	$(CC) $(LDFLAGS) -o $@ $^
	@echo "$(COLOR_GREEN)✓$(COLOR_RESET) Library built: $@"

# Build the test executable
$(EXEC_NAME): $(EXEC_OBJECTS) $(LIB_NAME)
	@echo "$(COLOR_BLUE)[LINKING]$(COLOR_RESET) Creating executable $@"
	$(CC) -o $@ $(EXEC_OBJECTS) -L. -lallocator -Wl,-rpath=.
	@chmod +x $@
	@echo "$(COLOR_GREEN)✓$(COLOR_RESET) Executable built: $@"

# Compile library object files
allocator.o: allocator.c allocator.h
	@echo "$(COLOR_BLUE)[COMPILE]$(COLOR_RESET) $<"
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Compile executable object files
runme.o: runme.c allocator.h
	@echo "$(COLOR_BLUE)[COMPILE]$(COLOR_RESET) $<"
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ============================================================================
# Test Targets (Required by Specification)
# ============================================================================

.PHONY: test
test: all
	@echo "$(COLOR_BLUE)════════════════════════════════════════$(COLOR_RESET)"
	@echo "$(COLOR_BLUE)Running Test Suite$(COLOR_RESET)"
	@echo "$(COLOR_BLUE)════════════════════════════════════════$(COLOR_RESET)"
	@echo ""
	@echo "$(COLOR_YELLOW)[TEST 1]$(COLOR_RESET) Clear Skies (Normal Operation)"
	@./$(EXEC_NAME) --seed $(TEST_SEED) --size $(TEST_SIZE) --storm 0 || true
	@echo ""
	@echo "$(COLOR_YELLOW)[TEST 2]$(COLOR_RESET) Light Storm (Low Radiation)"
	@./$(EXEC_NAME) --seed $(TEST_SEED) --size $(TEST_SIZE) --storm 1 || true
	@echo ""
	@echo "$(COLOR_YELLOW)[TEST 3]$(COLOR_RESET) Moderate Storm"
	@./$(EXEC_NAME) --seed $(TEST_SEED) --size $(TEST_SIZE) --storm 3 || true
	@echo ""
	@echo "$(COLOR_YELLOW)[TEST 4]$(COLOR_RESET) Heavy Storm"
	@./$(EXEC_NAME) --seed $(TEST_SEED) --size $(TEST_SIZE) --storm 5 || true
	@echo ""
	@echo "$(COLOR_YELLOW)[TEST 5]$(COLOR_RESET) Stress Test (Mixed Operations)"
	@./$(EXEC_NAME) --seed $(TEST_SEED) --size 16384 --stress 500 || true
	@echo ""
	@echo "$(COLOR_GREEN)════════════════════════════════════════$(COLOR_RESET)"
	@echo "$(COLOR_GREEN)Test suite completed!$(COLOR_RESET)"
	@echo "$(COLOR_GREEN)════════════════════════════════════════$(COLOR_RESET)"

# Quick test - basic functionality only
.PHONY: test-quick
test-quick: all
	@echo "$(COLOR_BLUE)[QUICK TEST]$(COLOR_RESET) Running basic tests..."
	@./$(EXEC_NAME) --seed $(TEST_SEED) --size $(TEST_SIZE)

# Test with various heap sizes
.PHONY: test-sizes
test-sizes: all
	@echo "$(COLOR_BLUE)[SIZE TEST]$(COLOR_RESET) Testing different heap sizes..."
	@echo "Testing 4KB heap..."
	@./$(EXEC_NAME) --size 4096 --seed $(TEST_SEED) || true
	@echo ""
	@echo "Testing 8KB heap..."
	@./$(EXEC_NAME) --size 8192 --seed $(TEST_SEED) || true
	@echo ""
	@echo "Testing 16KB heap..."
	@./$(EXEC_NAME) --size 16384 --seed $(TEST_SEED) || true
	@echo ""
	@echo "Testing 32KB heap..."
	@./$(EXEC_NAME) --size 32768 --seed $(TEST_SEED) || true

# Storm testing - progressively worse conditions
.PHONY: test-storm
test-storm: all
	@echo "$(COLOR_BLUE)[STORM TEST]$(COLOR_RESET) Testing radiation resilience..."
	@for level in 0 1 2 3 4 5 6 7 8 9 10; do \
		echo "$(COLOR_YELLOW)Storm Level $$level$(COLOR_RESET)"; \
		./$(EXEC_NAME) --seed $(TEST_SEED) --size $(TEST_SIZE) --storm $$level || true; \
		echo ""; \
	done

# Stress testing with increasing operations
.PHONY: test-stress
test-stress: all
	@echo "$(COLOR_BLUE)[STRESS TEST]$(COLOR_RESET) Testing under heavy load..."
	@echo "100 operations..."
	@./$(EXEC_NAME) --size 16384 --stress 100 --seed $(TEST_SEED) || true
	@echo ""
	@echo "500 operations..."
	@./$(EXEC_NAME) --size 16384 --stress 500 --seed $(TEST_SEED) || true
	@echo ""
	@echo "1000 operations..."
	@./$(EXEC_NAME) --size 32768 --stress 1000 --seed $(TEST_SEED) || true

# Test memory alignment
.PHONY: test-alignment
test-alignment: all
	@echo "$(COLOR_BLUE)[ALIGNMENT TEST]$(COLOR_RESET) Verifying 40-byte alignment..."
	@./$(EXEC_NAME) --seed 1 --size 8192
	@./$(EXEC_NAME) --seed 2 --size 8192
	@./$(EXEC_NAME) --seed 3 --size 8192

# Valgrind memory leak detection
.PHONY: test-valgrind
test-valgrind: all
	@echo "$(COLOR_BLUE)[VALGRIND]$(COLOR_RESET) Checking for memory leaks..."
	@if command -v valgrind >/dev/null 2>&1; then \
		valgrind --leak-check=full --show-leak-kinds=all \
		--track-origins=yes --verbose \
		./$(EXEC_NAME) --seed $(TEST_SEED) --size $(TEST_SIZE); \
	else \
		echo "$(COLOR_RED)Error: valgrind not installed$(COLOR_RESET)"; \
	fi

# ============================================================================
# Debug Targets
# ============================================================================

.PHONY: debug
debug: CFLAGS += $(DEBUG_FLAGS)
debug: clean all
	@echo "$(COLOR_YELLOW)Debug build complete$(COLOR_RESET)"
	@echo "Run with: ./$(EXEC_NAME)"

.PHONY: debug-gdb
debug-gdb: debug
	@echo "$(COLOR_BLUE)[GDB]$(COLOR_RESET) Starting debugger..."
	gdb --args ./$(EXEC_NAME) --seed $(TEST_SEED) --size $(TEST_SIZE)

# ============================================================================
# Code Quality Targets
# ============================================================================

.PHONY: check
check: all
	@echo "$(COLOR_BLUE)[CHECK]$(COLOR_RESET) Running static analysis..."
	@if command -v cppcheck >/dev/null 2>&1; then \
		cppcheck --enable=all --suppress=missingIncludeSystem \
		--inconclusive --std=c11 $(LIB_SOURCES) $(EXEC_SOURCES); \
	else \
		echo "$(COLOR_YELLOW)Warning: cppcheck not installed$(COLOR_RESET)"; \
	fi

.PHONY: format
format:
	@echo "$(COLOR_BLUE)[FORMAT]$(COLOR_RESET) Formatting source files..."
	@if command -v clang-format >/dev/null 2>&1; then \
		clang-format -i $(LIB_SOURCES) $(LIB_HEADERS) $(EXEC_SOURCES); \
		echo "$(COLOR_GREEN)✓$(COLOR_RESET) Files formatted"; \
	else \
		echo "$(COLOR_YELLOW)Warning: clang-format not installed$(COLOR_RESET)"; \
	fi

.PHONY: lint
lint:
	@echo "$(COLOR_BLUE)[LINT]$(COLOR_RESET) Checking code style..."
	@if command -v clang-tidy >/dev/null 2>&1; then \
		clang-tidy $(LIB_SOURCES) -- $(CFLAGS) $(INCLUDES); \
	else \
		echo "$(COLOR_YELLOW)Warning: clang-tidy not installed$(COLOR_RESET)"; \
	fi

# ============================================================================
# Clean Targets (Required by Specification)
# ============================================================================

.PHONY: clean
clean:
	@echo "$(COLOR_BLUE)[CLEAN]$(COLOR_RESET) Removing build artifacts..."
	@rm -f $(LIB_OBJECTS) $(EXEC_OBJECTS)
	@rm -f $(LIB_NAME) $(EXEC_NAME)
	@rm -f *.o *.so *.a
	@rm -f core vgcore.* *.core
	@rm -f *.log *.tmp
	@rm -f *.d
	@echo "$(COLOR_GREEN)✓$(COLOR_RESET) Clean complete"

.PHONY: distclean
distclean: clean
	@echo "$(COLOR_BLUE)[DISTCLEAN]$(COLOR_RESET) Removing all generated files..."
	@rm -rf *.dSYM
	@rm -f *~ *.bak *.swp
	@echo "$(COLOR_GREEN)✓$(COLOR_RESET) Distribution clean complete"

# ============================================================================
# Submission Targets
# ============================================================================

.PHONY: submission
submission: clean
	@echo "$(COLOR_BLUE)[SUBMISSION]$(COLOR_RESET) Creating submission package..."
	@mkdir -p submission
	@cp $(LIB_SOURCES) $(LIB_HEADERS) submission/
	@cp $(EXEC_SOURCES) submission/
	@cp Makefile submission/
	@echo "$(COLOR_GREEN)✓$(COLOR_RESET) Files copied to submission/"
	@echo ""
	@echo "$(COLOR_YELLOW)Checklist:$(COLOR_RESET)"
	@echo "  [ ] All .c and .h files included"
	@echo "  [ ] Makefile included"
	@echo "  [ ] Report PDF (max 4 pages)"
	@echo "  [ ] No subdirectories (all files in root)"
	@echo ""
	@echo "Create zip with: cd submission && zip -r ../submission.zip *"

.PHONY: check-submission
check-submission: submission
	@echo "$(COLOR_BLUE)[CHECK SUBMISSION]$(COLOR_RESET) Verifying submission package..."
	@cd submission && $(MAKE) all
	@cd submission && ./$(EXEC_NAME) --seed $(TEST_SEED) --size $(TEST_SIZE)
	@echo "$(COLOR_GREEN)✓$(COLOR_RESET) Submission package verified"

# ============================================================================
# Information Targets
# ============================================================================

.PHONY: help
help:
	@echo "$(COLOR_BLUE)════════════════════════════════════════$(COLOR_RESET)"
	@echo "$(COLOR_BLUE)Mars Allocator Makefile Help$(COLOR_RESET)"
	@echo "$(COLOR_BLUE)════════════════════════════════════════$(COLOR_RESET)"
	@echo ""
	@echo "$(COLOR_GREEN)Main Targets (Required):$(COLOR_RESET)"
	@echo "  make all           - Build library and executable (default)"
	@echo "  make test          - Run comprehensive test suite"
	@echo "  make clean         - Remove build artifacts"
	@echo ""
	@echo "$(COLOR_GREEN)Test Targets:$(COLOR_RESET)"
	@echo "  make test-quick    - Quick basic functionality test"
	@echo "  make test-sizes    - Test with different heap sizes"
	@echo "  make test-storm    - Test radiation resilience (levels 0-10)"
	@echo "  make test-stress   - Stress test with heavy operations"
	@echo "  make test-alignment - Verify alignment requirements"
	@echo "  make test-valgrind   - Check for memory leaks"
	@echo ""
	@echo "$(COLOR_GREEN)Debug Targets:$(COLOR_RESET)"
	@echo "  make debug         - Build with debug symbols and sanitizers"
	@echo "  make debug-gdb     - Build and run with GDB"
	@echo ""
	@echo "$(COLOR_GREEN)Code Quality:$(COLOR_RESET)"
	@echo "  make check         - Run static analysis (cppcheck)"
	@echo "  make format        - Format code (clang-format)"
	@echo "  make lint          - Check code style (clang-tidy)"
	@echo ""
	@echo "$(COLOR_GREEN)Submission:$(COLOR_RESET)"
	@echo "  make submission    - Prepare submission package"
	@echo "  make check-submission - Verify submission builds correctly"
	@echo ""
	@echo "$(COLOR_GREEN)Cleanup:$(COLOR_RESET)"
	@echo "  make clean         - Remove build files"
	@echo "  make distclean     - Remove all generated files"
	@echo ""
	@echo "$(COLOR_GREEN)Parameters (can override):$(COLOR_RESET)"
	@echo "  TEST_SEED=$(TEST_SEED)   - Random seed for tests"
	@echo "  TEST_SIZE=$(TEST_SIZE)   - Heap size for tests"
	@echo "  TEST_STORM=$(TEST_STORM) - Storm level (0-10)"
	@echo "  TEST_STRESS=$(TEST_STRESS) - Number of stress operations"
	@echo ""
	@echo "$(COLOR_GREEN)Examples:$(COLOR_RESET)"
	@echo "  make test TEST_SIZE=16384"
	@echo "  make test-storm TEST_SEED=12345"
	@echo "  make debug"
	@echo ""

.PHONY: info
info:
	@echo "$(COLOR_BLUE)Build Configuration:$(COLOR_RESET)"
	@echo "  Compiler:     $(CC)"
	@echo "  Flags:        $(CFLAGS)"
	@echo "  Library:      $(LIB_NAME)"
	@echo "  Executable:   $(EXEC_NAME)"
	@echo "  Sources:      $(LIB_SOURCES) $(EXEC_SOURCES)"
	@echo ""
	@echo "$(COLOR_BLUE)Test Configuration:$(COLOR_RESET)"
	@echo "  Seed:         $(TEST_SEED)"
	@echo "  Heap Size:    $(TEST_SIZE)"
	@echo "  Storm Level:  $(TEST_STORM)"
	@echo "  Stress Ops:   $(TEST_STRESS)"

# ============================================================================
# Utility Targets
# ============================================================================

.PHONY: run
run: all
	@./$(EXEC_NAME) --seed $(TEST_SEED) --size $(TEST_SIZE) --storm $(TEST_STORM)

.PHONY: run-storm
run-storm: all
	@./$(EXEC_NAME) --seed $(TEST_SEED) --size $(TEST_SIZE) --storm 5

.PHONY: run-stress
run-stress: all
	@./$(EXEC_NAME) --seed $(TEST_SEED) --size 16384 --stress 1000

# Show file sizes
.PHONY: size
size: all
	@echo "$(COLOR_BLUE)[SIZE]$(COLOR_RESET) Build artifact sizes:"
	@ls -lh $(LIB_NAME) $(EXEC_NAME)

# Count lines of code
.PHONY: loc
loc:
	@echo "$(COLOR_BLUE)[LOC]$(COLOR_RESET) Lines of code:"
	@wc -l $(LIB_SOURCES) $(LIB_HEADERS) $(EXEC_SOURCES) | tail -1

# ============================================================================
# Dependencies
# ============================================================================

# Automatic dependency generation
-include $(LIB_OBJECTS:.o=.d)
-include $(EXEC_OBJECTS:.o=.d)

%.d: %.c
	@$(CC) -MM $(CFLAGS) $(INCLUDES) $< > $@

# ============================================================================
# Special Rules
# ============================================================================

.SUFFIXES:
.SUFFIXES: .c .o .so

# Keep intermediate files
.PRECIOUS: %.o

# Prevent make from deleting intermediate files
.SECONDARY:

# Default goal
.DEFAULT_GOAL := all

# ============================================================================
# Notes
# ============================================================================
# This Makefile follows the COMP2221 specification requirements:
# - 'all' target builds liballocator.so and runme
# - 'test' target runs comprehensive tests
# - 'clean' target removes all build artifacts
# - No subdirectories in build output
# - Supports command-line parameters for testing
# ============================================================================