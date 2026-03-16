# r8e Development Guide

See [ARCHITECTURE.md](ARCHITECTURE.md) for the full technical specification.

## Quick Reference

**r8e** is a ~166KB JavaScript engine. C11, no dependencies, no JIT.

## Build

```
make release    # optimized build → build/libr8e.a
make debug      # with ASAN/UBSAN
make test       # run unit tests
make size       # minimum binary size
make bench      # run benchmarks
make clean      # remove build artifacts
```

## Project Structure

```
include/        # Public headers (r8e_types.h, r8e_opcodes.h, r8e_atoms.h, r8e_api.h)
src/            # Core engine sources (r8e_*.c)
src/security/   # Security layer (sandbox, arena, verifier, realm, capability)
src/ui/         # UI stack (DOM, style, layout, paint, events)
tests/          # Test runner + unit tests
bench/          # Benchmarks
```

## Key Design Decisions

- **NaN-boxing**: Every JS value in 8 bytes (section 2 of ARCHITECTURE.md)
- **4-tier object model**: 24B for 55% of objects (section 3)
- **Single-pass compiler**: No AST, <1.5x source memory (section 5)
- **RC + cycle detection**: Deterministic GC, 70-75% RC elision (section 8)
- **5-layer security**: OS sandbox → arena → verifier → realm → capabilities (section 11)
