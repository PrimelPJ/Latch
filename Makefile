# Convenience wrapper around CMake.
# Usage:
#   make              — release build
#   make debug        — debug + sanitisers
#   make test         — run unit tests
#   make bench        — run benchmarks (full)
#   make bench-quick  — run benchmarks (quick)
#   make clean        — remove build dirs

BUILD_DIR   := build
DEBUG_DIR   := build-debug
CMAKE       := cmake
BUILD_FLAGS := -j$(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

.PHONY: all debug test bench bench-quick clean

all: $(BUILD_DIR)/bench

$(BUILD_DIR)/CMakeCache.txt:
	@mkdir -p $(BUILD_DIR)
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release

$(BUILD_DIR)/run_tests $(BUILD_DIR)/bench: $(BUILD_DIR)/CMakeCache.txt
	$(CMAKE) --build $(BUILD_DIR) $(BUILD_FLAGS)

# ── Debug (ASan + TSan) ─────────────────────────────────────────────────
debug: $(DEBUG_DIR)/run_tests

$(DEBUG_DIR)/CMakeCache.txt:
	@mkdir -p $(DEBUG_DIR)
	$(CMAKE) -S . -B $(DEBUG_DIR) -DCMAKE_BUILD_TYPE=Debug

$(DEBUG_DIR)/run_tests: $(DEBUG_DIR)/CMakeCache.txt
	$(CMAKE) --build $(DEBUG_DIR) $(BUILD_FLAGS)

# ── Test ────────────────────────────────────────────────────────────────
test: $(BUILD_DIR)/run_tests
	@echo ""
	@echo "=== Unit tests (Release) ==="
	./$(BUILD_DIR)/run_tests

test-debug: $(DEBUG_DIR)/run_tests
	@echo ""
	@echo "=== Unit tests (Debug + sanitisers) ==="
	./$(DEBUG_DIR)/run_tests

# ── Bench ───────────────────────────────────────────────────────────────
bench: $(BUILD_DIR)/bench
	./$(BUILD_DIR)/bench

bench-quick: $(BUILD_DIR)/bench
	./$(BUILD_DIR)/bench --quick

# ── Clean ───────────────────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR) $(DEBUG_DIR)
