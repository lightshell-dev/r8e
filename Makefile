# ===========================================================================
# r8e JavaScript Engine - Build System
#
# See CLAUDE.md Section 13.2 (File Structure and Build System)
#
# Usage:
#   make              Build release binary
#   make debug        Build with sanitizers and debug symbols
#   make release      Build optimized release
#   make size         Build for minimum binary size
#   make test         Build and run unit tests
#   make bench        Build and run benchmarks
#   make clean        Remove all build artifacts
#   make format       Format source code (requires clang-format)
#
# SPDX-License-Identifier: MIT
# ===========================================================================

# --- Compiler Configuration ---
CC ?= cc
AR ?= ar
CFLAGS_BASE = -std=c11 -Wall -Wextra -Wpedantic -Wno-unused-parameter \
              -fno-strict-aliasing -Iinclude

# --- Platform Detection ---
UNAME_S := $(shell uname -s)
UNAME_M := $(shell uname -m)

ifeq ($(UNAME_S),Darwin)
  PLATFORM = macos
  CFLAGS_PLATFORM =
  LDFLAGS_PLATFORM =
  # macOS uses -dead_strip instead of --gc-sections
  LDFLAGS_SIZE = -Wl,-dead_strip
  # macOS may not have seccomp/landlock
  CFLAGS_SECURITY =
else ifeq ($(UNAME_S),Linux)
  PLATFORM = linux
  CFLAGS_PLATFORM =
  LDFLAGS_PLATFORM = -lm
  LDFLAGS_SIZE = -Wl,--gc-sections
  CFLAGS_SECURITY = -DR8E_HAS_SECCOMP -DR8E_HAS_LANDLOCK
else
  PLATFORM = other
  CFLAGS_PLATFORM =
  LDFLAGS_PLATFORM = -lm
  LDFLAGS_SIZE =
  CFLAGS_SECURITY =
endif

# --- Computed Goto Detection ---
# GCC and Clang support computed goto (labels as values)
ifneq (,$(findstring gcc,$(shell $(CC) --version 2>/dev/null)))
  CFLAGS_DISPATCH = -DR8E_COMPUTED_GOTO
else ifneq (,$(findstring clang,$(shell $(CC) --version 2>/dev/null)))
  CFLAGS_DISPATCH = -DR8E_COMPUTED_GOTO
else
  CFLAGS_DISPATCH =
endif

# --- Source Files ---
# Core engine sources
SRCS_CORE = \
	src/r8e_value.c \
	src/r8e_alloc.c \
	src/r8e_number.c \
	src/r8e_string.c \
	src/r8e_atom.c \
	src/r8e_parse.c \
	src/r8e_interp.c \
	src/r8e_object.c \
	src/r8e_array.c \
	src/r8e_function.c \
	src/r8e_closure.c \
	src/r8e_gc.c \
	src/r8e_regexp.c \
	src/r8e_error.c \
	src/r8e_module.c \
	src/r8e_builtin.c \
	src/r8e_json.c \
	src/r8e_promise.c \
	src/r8e_iterator.c \
	src/r8e_proxy.c \
	src/r8e_weakref.c

# Security module sources
SRCS_SECURITY = \
	src/security/r8e_sandbox.c \
	src/security/r8e_arena.c \
	src/security/r8e_verify.c \
	src/security/r8e_realm.c \
	src/security/r8e_capability.c

# UI module sources
SRCS_UI = \
	src/ui/r8e_dom.c \
	src/ui/r8e_style.c \
	src/ui/r8e_layout.c \
	src/ui/r8e_paint.c \
	src/ui/r8e_event.c

# All sources
SRCS = $(SRCS_CORE) $(SRCS_SECURITY) $(SRCS_UI)

# Headers
HDRS = \
	include/r8e_types.h \
	include/r8e_opcodes.h \
	include/r8e_atoms.h \
	include/r8e_api.h

# Test sources (r8e_test_stubs.c provides weak stubs for unimplemented symbols)
SRCS_TEST = tests/test_runner.c $(wildcard tests/unit/*.c) src/r8e_test_stubs.c

# Benchmark sources
SRCS_BENCH = bench/bench_runner.c

# --- Object Files ---
BUILD_DIR = build
OBJS = $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS))
OBJS_TEST = $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS_TEST))
OBJS_BENCH = $(patsubst %.c,$(BUILD_DIR)/%.o,$(SRCS_BENCH))

# --- Output ---
LIB = $(BUILD_DIR)/libr8e.a
BIN = $(BUILD_DIR)/r8e
TEST_BIN = $(BUILD_DIR)/r8e_test
BENCH_BIN = $(BUILD_DIR)/r8e_bench

# ===========================================================================
# Build Targets
# ===========================================================================

# Default: release build
.PHONY: all
all: release

# --- Release Build ---
.PHONY: release
release: CFLAGS = $(CFLAGS_BASE) -O2 -DNDEBUG $(CFLAGS_PLATFORM) \
                  $(CFLAGS_DISPATCH) $(CFLAGS_SECURITY)
release: LDFLAGS = $(LDFLAGS_PLATFORM)
release: $(LIB)
	@echo "=== r8e release build complete ==="
	@echo "  Library: $(LIB)"
	@ls -lh $(LIB) 2>/dev/null || true

# --- Debug Build (with sanitizers) ---
.PHONY: debug
debug: CFLAGS = $(CFLAGS_BASE) -g -O0 -DR8E_DEBUG \
                -fsanitize=address,undefined \
                $(CFLAGS_PLATFORM) $(CFLAGS_DISPATCH) $(CFLAGS_SECURITY)
debug: LDFLAGS = $(LDFLAGS_PLATFORM) -fsanitize=address,undefined
debug: $(LIB)
	@echo "=== r8e debug build complete (ASAN+UBSAN enabled) ==="

# --- Size-Optimized Build ---
.PHONY: size
size: CFLAGS = $(CFLAGS_BASE) -Oz -DNDEBUG -flto \
               -ffunction-sections -fdata-sections \
               $(CFLAGS_PLATFORM) $(CFLAGS_DISPATCH) $(CFLAGS_SECURITY)
size: LDFLAGS = $(LDFLAGS_PLATFORM) -flto $(LDFLAGS_SIZE)
size: $(LIB)
	@echo "=== r8e size-optimized build complete ==="
	@ls -lh $(LIB) 2>/dev/null || true

# --- Static Library ---
$(LIB): $(OBJS)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

# --- Object Compilation Rule ---
$(BUILD_DIR)/%.o: %.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# ===========================================================================
# Test Targets
# ===========================================================================

.PHONY: test
test: CFLAGS = $(CFLAGS_BASE) -g -O0 -DR8E_DEBUG -DR8E_TESTING \
               $(CFLAGS_PLATFORM) $(CFLAGS_DISPATCH)
test: LDFLAGS = $(LDFLAGS_PLATFORM)
test: $(TEST_BIN)
	@echo "=== Running r8e tests ==="
	./$(TEST_BIN)

$(TEST_BIN): $(OBJS) $(OBJS_TEST)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

# ===========================================================================
# Benchmark Targets
# ===========================================================================

.PHONY: bench
bench: CFLAGS = $(CFLAGS_BASE) -O2 -DNDEBUG $(CFLAGS_PLATFORM) \
                $(CFLAGS_DISPATCH)
bench: LDFLAGS = $(LDFLAGS_PLATFORM)
bench: $(BENCH_BIN)
	@echo "=== Running r8e benchmarks ==="
	./$(BENCH_BIN)

$(BENCH_BIN): $(OBJS) $(OBJS_BENCH)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@

# ===========================================================================
# Utility Targets
# ===========================================================================

.PHONY: clean
clean:
	rm -rf $(BUILD_DIR)
	@echo "=== Clean complete ==="

.PHONY: format
format:
	@if command -v clang-format >/dev/null 2>&1; then \
		find src include tests bench -name '*.c' -o -name '*.h' | \
			xargs clang-format -i --style=file; \
		echo "=== Format complete ==="; \
	else \
		echo "clang-format not found, skipping"; \
	fi

# Print binary size breakdown
.PHONY: sizes
sizes: size
	@echo ""
	@echo "=== Binary Size Breakdown ==="
	@for obj in $(OBJS); do \
		if [ -f "$$obj" ]; then \
			printf "  %6s  %s\n" "$$(wc -c < $$obj | tr -d ' ')" "$$obj"; \
		fi; \
	done | sort -rn
	@echo ""
	@echo "  Total library: $$(wc -c < $(LIB) | tr -d ' ') bytes"

# Show all source files and their line counts
.PHONY: stats
stats:
	@echo "=== Source Statistics ==="
	@echo "Core engine:"
	@wc -l $(SRCS_CORE) 2>/dev/null | tail -1 || echo "  (no source files yet)"
	@echo "Security:"
	@wc -l $(SRCS_SECURITY) 2>/dev/null | tail -1 || echo "  (no source files yet)"
	@echo "UI:"
	@wc -l $(SRCS_UI) 2>/dev/null | tail -1 || echo "  (no source files yet)"
	@echo "Headers:"
	@wc -l $(HDRS) 2>/dev/null | tail -1 || echo "  (no header files yet)"

# Check that headers compile cleanly on their own
.PHONY: check-headers
check-headers:
	@echo "=== Checking header self-containedness ==="
	@for hdr in $(HDRS); do \
		echo "  Checking $$hdr..."; \
		echo "#include \"$$hdr\"" | $(CC) $(CFLAGS_BASE) -fsyntax-only -x c - 2>&1 && \
			echo "    OK" || echo "    FAILED"; \
	done

# ===========================================================================
# Dependencies
# ===========================================================================

# Auto-dependency generation
DEPFLAGS = -MMD -MP
CFLAGS += $(DEPFLAGS)
-include $(OBJS:.o=.d)
-include $(OBJS_TEST:.o=.d)
-include $(OBJS_BENCH:.o=.d)
