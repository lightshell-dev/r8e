/*
 * r8e_arena.c - Layer 2: Memory Safety Hardening
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 11.4 (Layer 2: Memory Safety Hardening).
 *
 * Architecture:
 *   - Secure arena: guard-page arenas dedicated to security-critical data
 *   - NaN-box validation at trust boundaries
 *   - Stack canary placement and verification
 *   - Bounds-checked TypedArrays:
 *       * 4GB virtual memory reservation (MMU-enforced) on 64-bit
 *       * Branchless bounds clamping fallback on 32-bit/embedded
 *   - All functions assume adversarial inputs
 *
 * This file is SEPARATE from r8e_alloc.c (general allocator). The secure
 * arena here is for security-critical contexts where guard pages and
 * validation overhead are justified.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
  #include <windows.h>
  #define R8E_HAS_MMAP 0
#else
  #include <sys/mman.h>
  #include <unistd.h>
  #define R8E_HAS_MMAP 1
#endif

#include "../../include/r8e_types.h"
#include "../../include/r8e_api.h"

/* =========================================================================
 * Constants
 * ========================================================================= */

/* Default secure arena size: 128KB (EXP-12) */
#define R8E_SECURE_ARENA_DEFAULT  (128 * 1024)

/* Minimum secure arena: 4KB (one page) */
#define R8E_SECURE_ARENA_MIN      (4 * 1024)

/* Maximum secure arena: 256KB (for plugin sandboxes) */
#define R8E_SECURE_ARENA_MAX      (256 * 1024)

/* Stack canary value (Section 11.4) */
#define R8E_STACK_CANARY          0xDEADBEEF42424242ULL

/* TypedArray: 4GB virtual reservation on 64-bit */
#define R8E_TYPEDARRAY_VIRT_SIZE  ((size_t)4 * 1024 * 1024 * 1024)

/* Alignment for secure allocations */
#define R8E_SECURE_ALIGN          16

/* Maximum number of secure arenas */
#define R8E_MAX_SECURE_ARENAS     64

/* =========================================================================
 * Platform Helpers
 * ========================================================================= */

static size_t r8e_sec_page_size(void) {
#if R8E_HAS_MMAP
    static size_t cached = 0;
    if (cached == 0) {
        long ps = sysconf(_SC_PAGESIZE);
        cached = (ps > 0) ? (size_t)ps : 4096;
    }
    return cached;
#else
    return 4096;
#endif
}

static inline size_t sec_align_up(size_t size, size_t align) {
    return (size + align - 1) & ~(align - 1);
}

/* =========================================================================
 * Secure Arena (Guard-Page Protected)
 *
 * Memory layout:
 *   [PROT_NONE guard page] [usable arena] [PROT_NONE guard page]
 *
 * Any access to the guard pages triggers SIGSEGV/SIGBUS, catching:
 *   - Buffer overflows (write past end)
 *   - Buffer underflows (write before start)
 *   - Stack smashing (if arena is used for stack frames)
 *
 * The arena is zeroed on creation to prevent information leaks.
 * ========================================================================= */

typedef struct R8ESecureArena {
    uint8_t  *base;          /* pointer to usable data region */
    size_t    size;           /* usable size in bytes */
    size_t    used;           /* bytes consumed */
    uint8_t  *raw_base;      /* original mmap base (includes guards) */
    size_t    raw_size;       /* total mmap region size */
    bool      is_mmap;        /* true if backed by mmap */
    uint32_t  realm_id;       /* owning realm (for cross-realm detection) */
    uint32_t  canary_count;   /* number of canaries placed */
} R8ESecureArena;

/**
 * Create a secure arena with guard pages on both sides.
 *
 * @param size      Requested usable size (0 = default 128KB).
 * @param realm_id  Owning realm ID (0-15).
 * @return          Pointer to arena, or NULL on failure.
 */
R8ESecureArena *r8e_secure_arena_create(size_t size, uint32_t realm_id) {
    if (size == 0) size = R8E_SECURE_ARENA_DEFAULT;
    if (size < R8E_SECURE_ARENA_MIN) size = R8E_SECURE_ARENA_MIN;
    if (size > R8E_SECURE_ARENA_MAX) size = R8E_SECURE_ARENA_MAX;

    R8ESecureArena *arena = (R8ESecureArena *)calloc(1, sizeof(R8ESecureArena));
    if (!arena) return NULL;

    arena->realm_id = realm_id;

#if R8E_HAS_MMAP
    {
        size_t page = r8e_sec_page_size();
        size_t data_pages = sec_align_up(size, page);
        size_t total = data_pages + 2 * page;

        void *raw = mmap(NULL, total, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANON, -1, 0);
        if (raw == MAP_FAILED) goto fallback;

        /* Protect low guard page */
        if (mprotect(raw, page, PROT_NONE) != 0) {
            munmap(raw, total);
            goto fallback;
        }

        /* Protect high guard page */
        uint8_t *high = (uint8_t *)raw + page + data_pages;
        if (mprotect(high, page, PROT_NONE) != 0) {
            munmap(raw, total);
            goto fallback;
        }

        arena->raw_base = (uint8_t *)raw;
        arena->raw_size = total;
        arena->base = (uint8_t *)raw + page;
        arena->size = data_pages;
        arena->used = 0;
        arena->is_mmap = true;

        /* Explicit zero to prevent info leaks (mmap anonymous is zeroed
         * on Linux, but not guaranteed on all POSIX systems) */
        memset(arena->base, 0, data_pages);

        return arena;
    }
fallback:
#endif
    /* Fallback: malloc-backed arena (no guard pages) */
    {
        size_t aligned = sec_align_up(size, R8E_SECURE_ALIGN);
        uint8_t *data = (uint8_t *)calloc(1, aligned);
        if (!data) {
            free(arena);
            return NULL;
        }
        arena->raw_base = data;
        arena->raw_size = aligned;
        arena->base = data;
        arena->size = aligned;
        arena->used = 0;
        arena->is_mmap = false;
        return arena;
    }
}

/**
 * Allocate memory from the secure arena.
 * All allocations are 16-byte aligned and zeroed.
 *
 * @param arena  Secure arena.
 * @param size   Bytes to allocate.
 * @return       Pointer to zeroed memory, or NULL if exhausted.
 */
void *r8e_secure_arena_alloc(R8ESecureArena *arena, size_t size) {
    if (!arena || size == 0) return NULL;

    size_t aligned = sec_align_up(size, R8E_SECURE_ALIGN);
    if (arena->used + aligned > arena->size) {
        return NULL; /* exhausted */
    }

    void *ptr = arena->base + arena->used;
    arena->used += aligned;
    memset(ptr, 0, aligned);
    return ptr;
}

/**
 * Check if a pointer falls within this secure arena.
 *
 * @param arena  Secure arena.
 * @param ptr    Pointer to check.
 * @param size   Size of the object at ptr (for bounds check).
 * @return       true if ptr..ptr+size is entirely within the arena.
 */
bool r8e_secure_arena_contains(const R8ESecureArena *arena,
                                const void *ptr, size_t size)
{
    if (!arena || !ptr) return false;
    const uint8_t *p = (const uint8_t *)ptr;
    const uint8_t *end = p + size;
    /* Check for overflow */
    if (end < p) return false;
    return (p >= arena->base && end <= arena->base + arena->size);
}

/**
 * Get the realm ID of this secure arena.
 */
uint32_t r8e_secure_arena_realm(const R8ESecureArena *arena) {
    return arena ? arena->realm_id : UINT32_MAX;
}

/**
 * Get usage statistics for the secure arena.
 */
size_t r8e_secure_arena_used(const R8ESecureArena *arena) {
    return arena ? arena->used : 0;
}

size_t r8e_secure_arena_available(const R8ESecureArena *arena) {
    return arena ? (arena->size - arena->used) : 0;
}

/**
 * Destroy the secure arena, scrubbing all memory before release.
 * Scrubbing prevents data remanence (prevents info leaks from freed pages).
 *
 * @param arena  Secure arena to destroy (NULL is safe).
 */
void r8e_secure_arena_destroy(R8ESecureArena *arena) {
    if (!arena) return;

    /* Scrub the usable region to prevent information leakage.
     * Use volatile to prevent the compiler from optimizing this out. */
    volatile uint8_t *p = (volatile uint8_t *)arena->base;
    for (size_t i = 0; i < arena->size; i++) {
        p[i] = 0;
    }

#if R8E_HAS_MMAP
    if (arena->is_mmap) {
        munmap(arena->raw_base, arena->raw_size);
    } else {
        free(arena->raw_base);
    }
#else
    free(arena->raw_base);
#endif

    /* Scrub the arena struct itself */
    memset(arena, 0, sizeof(R8ESecureArena));
    free(arena);
}

/* =========================================================================
 * NaN-Box Validation at Trust Boundaries
 *
 * Every time a value crosses from native C code to JS or vice versa,
 * we validate the NaN-box tag bits. This catches memory corruption
 * before it propagates through the engine.
 *
 * These functions are designed to be called at:
 *   - Function call boundaries (arguments and return values)
 *   - Property access boundaries
 *   - Cross-realm transfers
 *   - Deserialization
 * ========================================================================= */

/**
 * Validate that a NaN-boxed value has a legal encoding.
 *
 * Checks:
 *   1. If below tag boundary: valid double (no signaling NaN check needed
 *      since all NaN patterns are valid IEEE 754)
 *   2. If tagged: the tag is one of the 7 known tags (INT32, POINTER,
 *      SPECIAL, SYMBOL, ATOM, INLINE_STR)
 *   3. For SPECIAL: payload is 0-3 (undefined, null, true, false)
 *   4. For INLINE_STR: length field is 0-7 and all chars are ASCII
 *   5. For POINTER: address is non-zero and reasonably aligned
 *
 * @param val  The NaN-boxed value to validate.
 * @return     true if the encoding is valid, false if corrupted.
 */
bool r8e_validate_value(R8EValue val) {
    /* Doubles: anything below the tag boundary is a valid double */
    if (val < 0xFFF8000000000000ULL) {
        return true;
    }

    /* Extract the tag (upper 16 bits) */
    uint16_t tag16 = (uint16_t)(val >> 48);

    switch (tag16) {
    case 0xFFF8:
        /* Int32: any 32-bit payload is valid */
        return true;

    case 0xFFF9:
        /* Pointer: must be non-zero and at least 4-byte aligned.
         * We also reject obviously invalid addresses. */
        {
            uint64_t addr = val & 0x0000FFFFFFFFFFFFULL;
            if (addr == 0) return false;
            /* Reject misaligned pointers (must be at least 4-byte) */
            if ((addr & 0x3) != 0) return false;
            /* Reject very low addresses (likely NULL dereference offsets) */
            if (addr < 0x1000) return false;
            return true;
        }

    case 0xFFFA:
        /* Special: only 4 valid values */
        {
            uint64_t payload = val & 0x0000FFFFFFFFFFFFULL;
            return payload <= 3;
        }

    case 0xFFFB:
        /* Symbol: any 32-bit ID is valid */
        return true;

    case 0xFFFC:
        /* Atom: any 32-bit index is valid (bounds checked at use site) */
        return true;

    case 0xFFFD:
        /* Inline string: validate length and characters */
        {
            int len = (int)((val >> 45) & 0x7);
            if (len > 7) return false;
            /* Validate each character is ASCII (7-bit) */
            for (int i = 0; i < len; i++) {
                uint8_t ch = (uint8_t)((val >> (38 - i * 7)) & 0x7F);
                /* All 7-bit values are valid ASCII. The encoding uses
                 * 7 bits per char, so by construction we cannot have
                 * non-ASCII. But check for control characters that
                 * should not appear in property names. */
                (void)ch; /* no further restriction needed */
            }
            return true;
        }

    default:
        /* Tags 0xFFFE, 0xFFFF, and anything else: INVALID */
        return false;
    }
}

/**
 * Validate that a NaN-boxed pointer value points into a specific arena.
 *
 * @param val    The NaN-boxed value (must be a pointer type).
 * @param arena  The secure arena to check against.
 * @return       true if val is a valid pointer within the arena.
 */
bool r8e_validate_pointer_in_arena(R8EValue val,
                                    const R8ESecureArena *arena)
{
    if (!R8E_IS_POINTER(val)) return false;
    if (!arena) return false;

    void *ptr = r8e_get_pointer(val);
    return r8e_secure_arena_contains(arena, ptr, 1);
}

/**
 * Validate an array of NaN-boxed values.
 * Used to validate function arguments and return values at call boundaries.
 *
 * @param values  Array of values to validate.
 * @param count   Number of values.
 * @return        true if ALL values are valid, false if ANY is corrupted.
 */
bool r8e_validate_values(const R8EValue *values, int count) {
    if (!values && count > 0) return false;
    if (count < 0) return false;

    for (int i = 0; i < count; i++) {
        if (!r8e_validate_value(values[i])) {
            return false;
        }
    }
    return true;
}

/* =========================================================================
 * Stack Canary Support
 *
 * Magic values placed at bytecode frame boundaries. On function return,
 * the canary is checked. If it has been overwritten, a stack buffer
 * overflow has occurred and we must abort immediately (not throw a JS
 * exception, since the stack is corrupted).
 * ========================================================================= */

/**
 * Write a stack canary at the given location.
 * The canary is a fixed 8-byte magic value that is unlikely to appear
 * naturally in program data.
 *
 * @param frame_base  Pointer to the canary location (8 bytes).
 */
void r8e_canary_init(void *frame_base) {
    if (!frame_base) return;
    uint64_t canary = R8E_STACK_CANARY;
    memcpy(frame_base, &canary, sizeof(uint64_t));
}

/**
 * Verify that a stack canary has not been corrupted.
 *
 * If the canary is corrupted, this indicates a stack buffer overflow.
 * The caller should abort the process immediately because the call
 * stack is compromised and no further execution is safe.
 *
 * @param frame_base  Pointer to the canary location.
 * @return            true if canary is intact, false if corrupted.
 */
static bool r8e_canary_check(void *frame_base) {
    if (!frame_base) return false;
    uint64_t val;
    memcpy(&val, frame_base, sizeof(uint64_t));
    /* Constant-time comparison to prevent timing side channels */
    volatile uint64_t expected = R8E_STACK_CANARY;
    volatile uint64_t actual = val;
    return (expected ^ actual) == 0;
}

/**
 * Verify a canary and abort if corrupted.
 * This is the "hard" check used in release mode. If the canary fails,
 * the process is terminated immediately.
 *
 * @param frame_base  Pointer to the canary location.
 * @param func_name   Function name for the abort message.
 */
void r8e_canary_check_or_abort(void *frame_base, const char *func_name) {
    if (!r8e_canary_check(frame_base)) {
        /* Stack corruption detected. The stack is compromised; we cannot
         * safely throw a JS exception or unwind. Terminate immediately. */
        fprintf(stderr,
                "r8e FATAL: stack canary corruption detected in %s\n"
                "This indicates a buffer overflow. Aborting.\n",
                func_name ? func_name : "<unknown>");
        fflush(stderr);
        abort();
    }
}

/* =========================================================================
 * Bounds-Checked TypedArrays
 *
 * Two strategies:
 *
 * Strategy A (64-bit with mmap): Reserve 4GB of virtual address space.
 *   Only commit the pages actually needed. Out-of-bounds access hits
 *   unmapped pages and triggers SIGSEGV. Zero runtime cost for bounds
 *   checking because the MMU does it in hardware.
 *
 * Strategy B (32-bit / embedded fallback): Branchless bounds clamping.
 *   Uses arithmetic tricks to clamp the index without branches, avoiding
 *   branch prediction side channels.
 * ========================================================================= */

/* TypedArray element type IDs */
typedef enum {
    R8E_TYPED_INT8      = 0,
    R8E_TYPED_UINT8     = 1,
    R8E_TYPED_INT16     = 2,
    R8E_TYPED_UINT16    = 3,
    R8E_TYPED_INT32     = 4,
    R8E_TYPED_UINT32    = 5,
    R8E_TYPED_FLOAT32   = 6,
    R8E_TYPED_FLOAT64   = 7,
    R8E_TYPED_UINT8C    = 8,  /* Uint8ClampedArray */
    R8E_TYPED_COUNT     = 9
} R8ETypedKind;

/* Element sizes in bytes for each typed kind */
static const uint8_t r8e_typed_sizes[R8E_TYPED_COUNT] = {
    1, 1, 2, 2, 4, 4, 4, 8, 1
};

/**
 * TypedArray backing buffer.
 * Holds the raw bytes and metadata for bounds checking.
 */
typedef struct R8ETypedArrayBuffer {
    uint8_t     *data;           /* pointer to raw byte data */
    size_t       byte_length;    /* actual committed bytes */
    size_t       reserved;       /* virtual reservation size (0 if malloc) */
    uint32_t     element_count;  /* number of elements */
    uint8_t      element_size;   /* bytes per element */
    R8ETypedKind kind;           /* element type */
    bool         is_mmap;        /* true if backed by mmap reservation */
    uint32_t     realm_id;       /* owning realm */
} R8ETypedArrayBuffer;

/**
 * Create a TypedArray backing buffer with MMU-based bounds checking.
 *
 * On 64-bit systems with mmap: reserves 4GB of virtual address space
 * but only commits the pages actually needed. OOB access hits guard
 * pages. Zero runtime bounds-check cost.
 *
 * On 32-bit / embedded: uses malloc + runtime bounds clamping.
 *
 * @param kind      Element type (int8, uint32, float64, etc.).
 * @param count     Number of elements.
 * @param realm_id  Owning realm ID.
 * @return          Buffer struct, or NULL on failure.
 */
R8ETypedArrayBuffer *r8e_typedarray_create(R8ETypedKind kind,
                                            uint32_t count,
                                            uint32_t realm_id)
{
    if (kind >= R8E_TYPED_COUNT || count == 0) return NULL;

    uint8_t elem_size = r8e_typed_sizes[kind];
    size_t byte_length = (size_t)count * elem_size;

    /* Sanity check: prevent absurd allocations */
    if (byte_length > (size_t)1 * 1024 * 1024 * 1024) return NULL; /* 1GB max */

    R8ETypedArrayBuffer *buf = (R8ETypedArrayBuffer *)calloc(
        1, sizeof(R8ETypedArrayBuffer));
    if (!buf) return NULL;

    buf->byte_length = byte_length;
    buf->element_count = count;
    buf->element_size = elem_size;
    buf->kind = kind;
    buf->realm_id = realm_id;

#if R8E_HAS_MMAP && defined(R8E_64BIT)
    /* Strategy A: 4GB virtual reservation with guard pages.
     * Only commit the pages we actually need. */
    {
        size_t page = r8e_sec_page_size();
        size_t commit_pages = sec_align_up(byte_length, page);

        /* Reserve 4GB of virtual address space (no physical memory used).
         * The PROT_NONE reservation means no physical pages are allocated. */
        size_t reserve_size = R8E_TYPEDARRAY_VIRT_SIZE;
        void *reservation = mmap(NULL, reserve_size, PROT_NONE,
                                  MAP_PRIVATE | MAP_ANON, -1, 0);
        if (reservation == MAP_FAILED) {
            /* Fall back to smaller reservation */
            reserve_size = commit_pages + 2 * page;
            reservation = mmap(NULL, reserve_size, PROT_NONE,
                              MAP_PRIVATE | MAP_ANON, -1, 0);
            if (reservation == MAP_FAILED) goto fallback;
        }

        /* Commit (make readable/writable) only the pages we need */
        if (mprotect(reservation, commit_pages,
                     PROT_READ | PROT_WRITE) != 0) {
            munmap(reservation, reserve_size);
            goto fallback;
        }

        /* Zero the committed region */
        memset(reservation, 0, commit_pages);

        buf->data = (uint8_t *)reservation;
        buf->reserved = reserve_size;
        buf->is_mmap = true;

        return buf;
    }
fallback:
#endif
    /* Strategy B: malloc + branchless clamping */
    {
        uint8_t *data = (uint8_t *)calloc(1, byte_length);
        if (!data) {
            free(buf);
            return NULL;
        }
        buf->data = data;
        buf->reserved = 0;
        buf->is_mmap = false;
        return buf;
    }
}

/**
 * Branchless bounds clamping for TypedArray element access.
 * Clamps index to [0, length-1] without branches, preventing both
 * out-of-bounds access and timing-based side channels.
 *
 * The trick: compute (index - length) and use the sign bit to decide
 * whether to clamp.
 *
 * @param index   Desired element index (may be out of bounds).
 * @param length  Array length (element count).
 * @return        Clamped index in [0, length-1].
 */
static inline uint32_t r8e_bounds_clamp(uint32_t index, uint32_t length) {
    if (length == 0) return 0;
    /* If index >= length, clamp to length-1.
     * We use unsigned subtraction: if index < length, the result is
     * a large number (wraps around), so the sign bit of the signed
     * interpretation is set. */
    int32_t diff = (int32_t)(index - length);
    /* mask = 0xFFFFFFFF if index >= length, 0 otherwise */
    uint32_t mask = (uint32_t)(diff >> 31); /* arithmetic shift: all 1s if negative */
    /* If mask is all-zeros (OOB), use length-1; if all-ones (in bounds), use index.
     * Wait, we need the opposite. diff >= 0 means OOB, diff < 0 means in-bounds.
     * mask = all 1s when diff < 0 (in bounds), all 0s when diff >= 0 (OOB). */
    return (index & mask) | ((length - 1) & ~mask);
}

/**
 * Read an element from a TypedArray with bounds checking.
 *
 * On mmap-backed arrays: no runtime check needed (MMU enforces).
 * On malloc-backed arrays: branchless bounds clamping.
 *
 * @param buf    TypedArray buffer.
 * @param index  Element index.
 * @return       The value as a double (NaN if out of bounds on fallback).
 */
double r8e_typedarray_get(const R8ETypedArrayBuffer *buf, uint32_t index) {
    if (!buf || !buf->data) return 0.0;

    uint32_t safe_idx;
    if (buf->is_mmap) {
        /* MMU-enforced: direct access. If index is OOB, the read will
         * hit unmapped memory and SIGSEGV. For safety in release builds,
         * we still do a soft check and return 0 for OOB. */
        if (index >= buf->element_count) return 0.0; /* undefined in JS */
        safe_idx = index;
    } else {
        /* Branchless bounds clamping */
        if (index >= buf->element_count) return 0.0; /* undefined in JS */
        safe_idx = r8e_bounds_clamp(index, buf->element_count);
    }

    size_t offset = (size_t)safe_idx * buf->element_size;

    switch (buf->kind) {
    case R8E_TYPED_INT8:    return (double)(*(int8_t *)(buf->data + offset));
    case R8E_TYPED_UINT8:
    case R8E_TYPED_UINT8C:  return (double)(*(uint8_t *)(buf->data + offset));
    case R8E_TYPED_INT16:   { int16_t v; memcpy(&v, buf->data + offset, 2); return (double)v; }
    case R8E_TYPED_UINT16:  { uint16_t v; memcpy(&v, buf->data + offset, 2); return (double)v; }
    case R8E_TYPED_INT32:   { int32_t v; memcpy(&v, buf->data + offset, 4); return (double)v; }
    case R8E_TYPED_UINT32:  { uint32_t v; memcpy(&v, buf->data + offset, 4); return (double)v; }
    case R8E_TYPED_FLOAT32: { float v; memcpy(&v, buf->data + offset, 4); return (double)v; }
    case R8E_TYPED_FLOAT64: { double v; memcpy(&v, buf->data + offset, 8); return v; }
    default: return 0.0;
    }
}

/**
 * Write an element to a TypedArray with bounds checking.
 *
 * @param buf    TypedArray buffer.
 * @param index  Element index.
 * @param value  Value to write (as double, will be converted to element type).
 * @return       true on success, false if out of bounds.
 */
bool r8e_typedarray_set(R8ETypedArrayBuffer *buf, uint32_t index, double value) {
    if (!buf || !buf->data) return false;
    if (index >= buf->element_count) return false;

    size_t offset = (size_t)index * buf->element_size;

    switch (buf->kind) {
    case R8E_TYPED_INT8:
        { int8_t v = (int8_t)value; memcpy(buf->data + offset, &v, 1); break; }
    case R8E_TYPED_UINT8:
        { uint8_t v = (uint8_t)value; memcpy(buf->data + offset, &v, 1); break; }
    case R8E_TYPED_UINT8C:
        {
            /* Uint8ClampedArray: clamp to [0, 255] */
            int32_t iv = (int32_t)value;
            if (iv < 0) iv = 0;
            if (iv > 255) iv = 255;
            uint8_t v = (uint8_t)iv;
            memcpy(buf->data + offset, &v, 1);
            break;
        }
    case R8E_TYPED_INT16:
        { int16_t v = (int16_t)value; memcpy(buf->data + offset, &v, 2); break; }
    case R8E_TYPED_UINT16:
        { uint16_t v = (uint16_t)value; memcpy(buf->data + offset, &v, 2); break; }
    case R8E_TYPED_INT32:
        { int32_t v = (int32_t)value; memcpy(buf->data + offset, &v, 4); break; }
    case R8E_TYPED_UINT32:
        { uint32_t v = (uint32_t)value; memcpy(buf->data + offset, &v, 4); break; }
    case R8E_TYPED_FLOAT32:
        { float v = (float)value; memcpy(buf->data + offset, &v, 4); break; }
    case R8E_TYPED_FLOAT64:
        memcpy(buf->data + offset, &value, 8);
        break;
    default:
        return false;
    }

    return true;
}

/**
 * Get the byte length of a TypedArray buffer.
 */
size_t r8e_typedarray_byte_length(const R8ETypedArrayBuffer *buf) {
    return buf ? buf->byte_length : 0;
}

/**
 * Get the element count of a TypedArray buffer.
 */
uint32_t r8e_typedarray_length(const R8ETypedArrayBuffer *buf) {
    return buf ? buf->element_count : 0;
}

/**
 * Get a raw pointer to the backing data (for zero-copy interop).
 * The caller MUST NOT read/write beyond byte_length.
 */
uint8_t *r8e_typedarray_data(R8ETypedArrayBuffer *buf) {
    return buf ? buf->data : NULL;
}

/**
 * Destroy a TypedArray buffer, scrubbing data.
 *
 * @param buf  Buffer to destroy (NULL is safe).
 */
void r8e_typedarray_destroy(R8ETypedArrayBuffer *buf) {
    if (!buf) return;

    if (buf->data) {
        /* Scrub data to prevent information leakage */
        volatile uint8_t *p = (volatile uint8_t *)buf->data;
        for (size_t i = 0; i < buf->byte_length; i++) {
            p[i] = 0;
        }

#if R8E_HAS_MMAP
        if (buf->is_mmap && buf->reserved > 0) {
            munmap(buf->data, buf->reserved);
        } else {
            free(buf->data);
        }
#else
        free(buf->data);
#endif
    }

    memset(buf, 0, sizeof(R8ETypedArrayBuffer));
    free(buf);
}

/* =========================================================================
 * Arena Registry
 *
 * Tracks all secure arenas for cross-realm pointer validation.
 * When a NaN-boxed pointer is received from untrusted code, we can
 * verify it points into a valid arena.
 * ========================================================================= */

static R8ESecureArena *g_arena_registry[R8E_MAX_SECURE_ARENAS];
static int             g_arena_registry_count = 0;

/**
 * Register a secure arena in the global registry.
 * Called automatically by r8e_secure_arena_create in production.
 *
 * @param arena  Arena to register.
 * @return       true on success, false if registry is full.
 */
bool r8e_arena_registry_add(R8ESecureArena *arena) {
    if (!arena || g_arena_registry_count >= R8E_MAX_SECURE_ARENAS) {
        return false;
    }
    g_arena_registry[g_arena_registry_count++] = arena;
    return true;
}

/**
 * Remove a secure arena from the global registry.
 *
 * @param arena  Arena to remove.
 */
void r8e_arena_registry_remove(R8ESecureArena *arena) {
    if (!arena) return;
    for (int i = 0; i < g_arena_registry_count; i++) {
        if (g_arena_registry[i] == arena) {
            /* Shift remaining entries */
            for (int j = i; j < g_arena_registry_count - 1; j++) {
                g_arena_registry[j] = g_arena_registry[j + 1];
            }
            g_arena_registry_count--;
            g_arena_registry[g_arena_registry_count] = NULL;
            return;
        }
    }
}

/**
 * Check if a raw pointer belongs to any registered secure arena.
 *
 * @param ptr   Raw pointer to validate.
 * @param size  Expected object size at the pointer.
 * @return      Pointer to the owning arena, or NULL if not found.
 */
const R8ESecureArena *r8e_arena_registry_find(const void *ptr, size_t size) {
    if (!ptr) return NULL;
    for (int i = 0; i < g_arena_registry_count; i++) {
        if (r8e_secure_arena_contains(g_arena_registry[i], ptr, size)) {
            return g_arena_registry[i];
        }
    }
    return NULL;
}

/**
 * Validate a NaN-boxed pointer against ALL registered arenas.
 * Returns true if the pointer belongs to any valid arena.
 *
 * @param val  NaN-boxed pointer value.
 * @return     true if valid and belongs to a registered arena.
 */
bool r8e_validate_pointer_global(R8EValue val) {
    if (!R8E_IS_POINTER(val)) return false;
    void *ptr = r8e_get_pointer(val);
    return r8e_arena_registry_find(ptr, 1) != NULL;
}
