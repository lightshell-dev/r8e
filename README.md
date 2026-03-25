# r8e

**r(unmachin)e** — a secure, embeddable JavaScript engine in C11.

[![Build](https://github.com/lightshell-dev/r8e/actions/workflows/ci.yml/badge.svg)](https://github.com/lightshell-dev/r8e/actions)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

r8e is a from-scratch JavaScript engine that replaces V8 (10M LOC, ~60MB) and SpiderMonkey (4M LOC, ~50MB) with a minimal, secure alternative. No JIT compiler. Pure bytecode interpreter. Deterministic garbage collection. Five-layer security architecture. Built-in UI rendering stack. Zero external dependencies.

Designed for desktop applications, embedded systems, sandboxed plugin execution, and lightweight UI applications where binary size, memory usage, and security matter more than raw throughput.

## Key Properties

| | r8e | V8 | QuickJS | Hermes |
|---|---|---|---|---|
| **Binary size** | ~170KB | ~60MB | ~1.5MB | ~3MB |
| **Runtime memory** (50KB app) | 59KB | — | 136KB | — |
| **Dependencies** | 0 | many | 0 | many |
| **JIT** | No | Yes | No | No |
| **Security layers** | 5 | 1 | 0 | 0 |
| **Built-in UI** | Yes | No | No | No |

## Quick Start

```c
#include "r8e_api.h"

int main(void) {
    R8EContext *ctx = r8e_context_new();

    // Evaluate JavaScript
    R8EValue result = r8e_eval(ctx, "2 + 2", 0);
    // result is 4

    // Register native functions
    r8e_set_global(ctx, "answer", r8e_from_int32(42));
    r8e_eval(ctx, "answer + 8", 0);  // 50

    // Sandboxed execution in isolated realms
    int realm = r8e_realm_new(ctx);
    r8e_realm_switch(ctx, realm);
    r8e_eval(ctx, "untrusted_code()", 0);
    r8e_realm_destroy(ctx, realm);

    r8e_context_free(ctx);
    return 0;
}
```

## Building

```bash
make release        # optimized build → build/libr8e.a
make test           # run 1500+ unit tests
make debug          # ASAN + UBSAN enabled
make size           # minimal binary (-Oz, LTO)
make bench          # run benchmarks
```

Requirements: C11 compiler (GCC 7+ or Clang 5+). No external dependencies.

## Platform Support

| Platform | Status | Notes |
|----------|--------|-------|
| macOS (arm64, x86_64) | Supported | Metal GPU rendering |
| Linux (x86_64, aarch64) | Supported | Full security stack (seccomp, Landlock) |
| Windows (MSVC, MinGW) | Supported | Switch-based dispatch fallback |
| FreeBSD | Supported | pledge-style sandbox |

## Architecture

```
Source → Lexer → Single-Pass Compiler → Bytecode → Verifier → Interpreter
                  (no AST)              (4-bit prefix)  (O(n))   (computed goto)
```

### NaN-Boxing

Every JS value fits in 8 bytes. Doubles extract at zero cost. Strings up to 7 ASCII characters encode inline with no heap allocation.

### 4-Tier Object Model

98.8% of JavaScript objects have 4 or fewer properties:

| Tier | Properties | Size | Coverage |
|------|-----------|------|----------|
| 0 | 0-1 | 24 bytes | 55% |
| 1 | 2-4 | 40-72 bytes | 44% |
| 2 | 5-16 | heap array | ~1% |
| 3 | 17+ | Robin Hood hash | <0.01% |

### Deterministic GC

Reference counting with compile-time elision. The compiler classifies values as OWNED, BORROWED, or TEMP — 70-75% of RC operations are eliminated at compile time. Epoch-based cycle detection handles circular references.

### Security (5 Layers)

| Layer | Mechanism | Defends Against |
|-------|-----------|-----------------|
| 1 | OS sandbox (seccomp-BPF, pledge, Landlock) | Code execution exploits |
| 2 | Memory hardening (guard pages, canaries) | Buffer overflows |
| 3 | Bytecode verifier (O(n) pre-execution) | Malformed bytecode |
| 4 | Realm isolation (64KB per realm, <2us creation) | Cross-context attacks |
| 5 | Capability-based API | Ambient authority abuse |

### Hardened Font Rasterizer

Built-in TrueType font parser and rasterizer with bounds-checked reads on every byte of font data. Quadratic Bezier scanline rasterizer with 4x supersampling. Bundled Inter and Open Sans fonts. Safe to use on untrusted font files — malformed fonts return errors, never crash.

### UI Rendering Stack

A minimal rendering pipeline in ~28KB:

- **DOM** — element tree, selectors, class lists, innerHTML, querySelector
- **Style** — CSS subset (~60 properties), selector matching via atom interning
- **Layout** — flexbox + block layout
- **Paint** — display list generation (consumed by Metal/Vulkan GPU backends)
- **Event** — W3C event dispatch, bubbling/capture, hit testing

### AI Primitives

Built-in primitives for LLM integration:

- **GGUF parser** — zero-copy mmap tensor data access
- **BPE tokenizer** — text ↔ token ID conversion
- **Sampler** — temperature, top-k, top-p, repetition penalty
- **KV cache** — float16 key-value storage for transformer layers
- **Chat templates** — ChatML, LLaMA, Phi-3 prompt formatting
- **SSE parser** — streaming API response parsing
- **MCP** — JSON-RPC 2.0, stdio transport, session management

## C API

```c
// Context lifecycle
R8EContext *r8e_context_new(void);
void        r8e_context_free(R8EContext *ctx);

// Evaluation
R8EValue r8e_eval(R8EContext *ctx, const char *source, size_t len);
R8EValue r8e_call(R8EContext *ctx, R8EValue func, R8EValue this_val,
                   int argc, const R8EValue *argv);

// Value creation
R8EValue r8e_make_object(R8EContext *ctx);
R8EValue r8e_make_array(R8EContext *ctx, uint32_t capacity);
R8EValue r8e_make_cstring(R8EContext *ctx, const char *str);
R8EValue r8e_make_native_func(R8EContext *ctx, R8ENativeFunc fn,
                               const char *name, int argc);

// Property access
R8EValue  r8e_get_prop(R8EContext *ctx, R8EValue obj, const char *name);
R8EStatus r8e_set_prop(R8EContext *ctx, R8EValue obj, const char *name, R8EValue val);
R8EStatus r8e_define_accessor(R8EContext *ctx, R8EValue obj, const char *name,
                                R8EValue getter, R8EValue setter);

// Globals
R8EStatus r8e_set_global(R8EContext *ctx, const char *name, R8EValue val);
R8EStatus r8e_set_global_func(R8EContext *ctx, const char *name,
                                R8ENativeFunc fn, int argc);

// Font rasterizer
R8EFont *r8e_font_load(const uint8_t *data, uint32_t length);
R8EFont *r8e_font_load_default(void);   // bundled Inter
uint32_t r8e_font_glyph_id(R8EFont *font, uint32_t codepoint);
bool     r8e_font_rasterize(R8EFont *font, uint32_t glyph_id, float scale,
                              R8EGlyphBitmap *out);
```

See [r8e_api.h](include/r8e_api.h) for the complete API reference.

## Testing

1500+ test functions across 39+ test suites covering value encoding, parsing, object model, garbage collection, closures, interpreter dispatch, promises, proxies, regular expressions, JSON, modules, security boundaries, font rasterization, UI rendering, and AI primitives.

```bash
make test           # run all tests
make debug          # build with sanitizers
```

## Project Structure

```
include/            Public headers
src/                Core engine
src/security/       5-layer security stack
src/ui/             UI rendering (DOM, style, layout, paint, events, bridge)
src/gpu/            Display list format and arena allocator
src/ai/             AI primitives (GGUF, tokenizer, sampler, KV cache)
src/mcp/            MCP integration (JSON-RPC, stdio, session)
tests/              Unit and integration tests
bench/              Benchmarks
```

## Used By

- **[LightShell](https://github.com/lightshell-dev/lightshell)** — Desktop app framework powered by r8e

## Contributing

Contributions welcome. Please ensure all tests pass before submitting a PR.

```bash
make test           # must pass
make debug          # develop with sanitizers
```

## License

MIT

## Acknowledgments

Informed by the designs of QuickJS (Fabrice Bellard), V8 (Google), SpiderMonkey (Mozilla), and Hermes (Meta). The UI layout model draws from Yoga (Meta).
