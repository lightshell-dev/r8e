/*
 * r8e_alloc.c - Arena allocator with guard pages and slab allocator
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Sections 8 (GC), 11.4 (Memory Safety Hardening).
 *
 * Architecture:
 *   - Arena allocator: bump-pointer allocation within mmap'd region
 *   - Guard pages on both ends: overflow/underflow -> immediate SIGSEGV
 *   - 128KB default arena size (EXP-12)
 *   - Slab allocator for common object sizes (24, 40, 72 bytes)
 *   - Memory tracking: current and peak usage
 *   - macOS + Linux compatible (mmap/mprotect)
 *   - Fallback to malloc when mmap is unavailable
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
/* Windows fallback: use VirtualAlloc */
#include <windows.h>
#define R8E_HAS_MMAP 0
#else
/* POSIX: use mmap/mprotect */
#include <sys/mman.h>
#include <unistd.h>
#define R8E_HAS_MMAP 1
#endif

/* -------------------------------------------------------------------------
 * Minimal type definitions (standalone until r8e_types.h arrives)
 * ------------------------------------------------------------------------- */
#ifndef R8E_TYPES_H
typedef uint64_t R8EValue;
#endif

/* -------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------- */

/* Default arena size: 128KB (EXP-12 optimal) */
#define R8E_ARENA_DEFAULT_SIZE   (128 * 1024)

/* Minimum arena size: 4KB */
#define R8E_ARENA_MIN_SIZE       (4 * 1024)

/* Maximum arena size: 16MB (sanity limit) */
#define R8E_ARENA_MAX_SIZE       (16 * 1024 * 1024)

/* Alignment for all allocations (8 bytes = NaN-box aligned) */
#define R8E_ALLOC_ALIGN          8

/* Slab sizes matching object tier sizes (Section 3) */
#define R8E_SLAB_SIZE_TIER0      24   /* Tier 0: Micro Object */
#define R8E_SLAB_SIZE_TIER1_SM   40   /* Tier 1: 2 properties */
#define R8E_SLAB_SIZE_TIER1_LG   72   /* Tier 1: 4 properties */

/* Number of slab size classes */
#define R8E_SLAB_NUM_CLASSES     3

/* Slab block size: each slab block holds this many objects */
#define R8E_SLAB_BLOCK_COUNT     64

/* Stack canary value (Section 11.4) */
#define R8E_CANARY_VALUE         0xDEADBEEF42424242ULL

/* -------------------------------------------------------------------------
 * Platform helpers
 * ------------------------------------------------------------------------- */

static size_t r8e_page_size(void) {
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

/* Align a size up to the given alignment */
static inline size_t align_up(size_t size, size_t align) {
    return (size + align - 1) & ~(align - 1);
}

/* =========================================================================
 * ARENA ALLOCATOR
 * ========================================================================= */

/*
 * R8EArena - Bump-pointer arena with guard pages
 *
 * Memory layout (when mmap available):
 *   [guard page] [arena data ...] [guard page]
 *
 * The guard pages are mprotect'd PROT_NONE, so any access to them
 * triggers SIGSEGV. This catches buffer overflows and underflows.
 *
 * Allocation is simple bump-pointer: advance the "used" offset,
 * return pointer into the data region. O(1) allocation, zero fragmentation.
 * Reset is O(1): just set used = 0.
 */
typedef struct R8EArena {
    uint8_t *base;          /* pointer to start of usable data */
    size_t   size;          /* total usable size in bytes */
    size_t   used;          /* bytes allocated so far */
    size_t   peak_used;     /* high-water mark */
    uint8_t *raw_base;      /* original mmap base (includes guard pages) */
    size_t   raw_size;      /* total mmap size (includes guard pages) */
    bool     is_mmap;       /* true if backed by mmap, false if malloc */
    uint32_t alloc_count;   /* number of allocations (for stats) */
    uint32_t reset_count;   /* number of resets (for stats) */
} R8EArena;

/*
 * Create a new arena with guard pages.
 *
 * @param size  Requested arena size in bytes (0 = default 128KB).
 *              Will be rounded up to page alignment.
 * @return      Pointer to arena struct, or NULL on failure.
 *
 * Memory layout with mmap:
 *   [1 page PROT_NONE] [arena pages PROT_READ|PROT_WRITE] [1 page PROT_NONE]
 */
R8EArena *r8e_arena_create(size_t size) {
    if (size == 0) size = R8E_ARENA_DEFAULT_SIZE;
    if (size < R8E_ARENA_MIN_SIZE) size = R8E_ARENA_MIN_SIZE;
    if (size > R8E_ARENA_MAX_SIZE) size = R8E_ARENA_MAX_SIZE;

    R8EArena *arena = (R8EArena *)calloc(1, sizeof(R8EArena));
    if (!arena) return NULL;

#if R8E_HAS_MMAP
    size_t page = r8e_page_size();
    size_t arena_pages = align_up(size, page);
    size_t total = arena_pages + 2 * page; /* data + 2 guard pages */

    /* Map the entire region (including guard pages) */
    void *raw = mmap(NULL, total, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON, -1, 0);
    if (raw == MAP_FAILED) {
        /* Fallback to malloc */
        goto malloc_fallback;
    }

    /* Protect the low guard page */
    if (mprotect(raw, page, PROT_NONE) != 0) {
        munmap(raw, total);
        goto malloc_fallback;
    }

    /* Protect the high guard page */
    uint8_t *high_guard = (uint8_t *)raw + page + arena_pages;
    if (mprotect(high_guard, page, PROT_NONE) != 0) {
        munmap(raw, total);
        goto malloc_fallback;
    }

    arena->raw_base = (uint8_t *)raw;
    arena->raw_size = total;
    arena->base = (uint8_t *)raw + page; /* skip low guard */
    arena->size = arena_pages;
    arena->is_mmap = true;
    arena->used = 0;
    arena->peak_used = 0;
    arena->alloc_count = 0;
    arena->reset_count = 0;

    return arena;

malloc_fallback:
#endif
    /* Fallback: plain malloc, no guard pages */
    {
        size_t aligned_size = align_up(size, R8E_ALLOC_ALIGN);
        uint8_t *data = (uint8_t *)malloc(aligned_size);
        if (!data) {
            free(arena);
            return NULL;
        }
        arena->raw_base = data;
        arena->raw_size = aligned_size;
        arena->base = data;
        arena->size = aligned_size;
        arena->is_mmap = false;
        arena->used = 0;
        arena->peak_used = 0;
        arena->alloc_count = 0;
        arena->reset_count = 0;
        return arena;
    }
}

/*
 * Allocate memory from the arena.
 *
 * @param arena  The arena to allocate from.
 * @param size   Number of bytes to allocate.
 * @return       Pointer to allocated memory (8-byte aligned), or NULL if
 *               the arena is exhausted.
 *
 * O(1) operation: just bumps the offset. Zero fragmentation.
 */
void *r8e_arena_alloc(R8EArena *arena, size_t size) {
    if (!arena || size == 0) return NULL;

    size_t aligned = align_up(size, R8E_ALLOC_ALIGN);
    if (arena->used + aligned > arena->size) {
        return NULL; /* arena exhausted */
    }

    void *ptr = arena->base + arena->used;
    arena->used += aligned;
    arena->alloc_count++;

    if (arena->used > arena->peak_used) {
        arena->peak_used = arena->used;
    }

    /* Zero the memory (security: prevent info leaks) */
    memset(ptr, 0, aligned);

    return ptr;
}

/*
 * Allocate memory from the arena, without zeroing.
 * Use when caller will immediately overwrite all bytes.
 */
void *r8e_arena_alloc_unzeroed(R8EArena *arena, size_t size) {
    if (!arena || size == 0) return NULL;

    size_t aligned = align_up(size, R8E_ALLOC_ALIGN);
    if (arena->used + aligned > arena->size) {
        return NULL;
    }

    void *ptr = arena->base + arena->used;
    arena->used += aligned;
    arena->alloc_count++;

    if (arena->used > arena->peak_used) {
        arena->peak_used = arena->used;
    }

    return ptr;
}

/*
 * Reset the arena to empty state.
 * All previously allocated pointers become invalid.
 * O(1) operation: just resets the offset.
 */
void r8e_arena_reset(R8EArena *arena) {
    if (!arena) return;
    arena->used = 0;
    arena->alloc_count = 0;
    arena->reset_count++;
}

/*
 * Destroy the arena, freeing all memory.
 */
void r8e_arena_destroy(R8EArena *arena) {
    if (!arena) return;

#if R8E_HAS_MMAP
    if (arena->is_mmap) {
        /* Need to make guard pages writable before unmapping on some systems,
         * but munmap doesn't require this. Just unmap the whole region. */
        munmap(arena->raw_base, arena->raw_size);
    } else {
        free(arena->raw_base);
    }
#else
    free(arena->raw_base);
#endif

    arena->base = NULL;
    arena->raw_base = NULL;
    arena->size = 0;
    arena->used = 0;
    free(arena);
}

/*
 * Query arena statistics.
 */
size_t r8e_arena_used(const R8EArena *arena) {
    return arena ? arena->used : 0;
}

size_t r8e_arena_available(const R8EArena *arena) {
    return arena ? (arena->size - arena->used) : 0;
}

size_t r8e_arena_size(const R8EArena *arena) {
    return arena ? arena->size : 0;
}

size_t r8e_arena_peak(const R8EArena *arena) {
    return arena ? arena->peak_used : 0;
}

uint32_t r8e_arena_alloc_count(const R8EArena *arena) {
    return arena ? arena->alloc_count : 0;
}

bool r8e_arena_contains(const R8EArena *arena, const void *ptr) {
    if (!arena || !ptr) return false;
    const uint8_t *p = (const uint8_t *)ptr;
    return (p >= arena->base && p < arena->base + arena->size);
}

/* =========================================================================
 * SLAB ALLOCATOR
 *
 * Fixed-size block allocator for common object sizes.
 * Uses a free list for O(1) alloc/free. No fragmentation for same-size
 * objects. Each slab block holds SLAB_BLOCK_COUNT objects.
 * ========================================================================= */

/*
 * Free list node: when a slab slot is free, it contains a pointer to
 * the next free slot. When allocated, the entire slot is available to
 * the user.
 */
typedef struct R8ESlabFreeNode {
    struct R8ESlabFreeNode *next;
} R8ESlabFreeNode;

/*
 * A single slab block: a contiguous region holding SLAB_BLOCK_COUNT
 * objects of a fixed size.
 */
typedef struct R8ESlabBlock {
    struct R8ESlabBlock *next_block; /* linked list of blocks */
    uint8_t            *data;        /* raw memory for objects */
    uint32_t            capacity;    /* how many objects fit */
    uint32_t            obj_size;    /* size of each object (aligned) */
} R8ESlabBlock;

/*
 * R8ESlab - Slab allocator for a specific object size.
 *
 * Maintains a free list of available slots. When the free list is empty,
 * allocates a new block.
 */
typedef struct R8ESlab {
    R8ESlabFreeNode *free_list;     /* head of free list */
    R8ESlabBlock    *blocks;        /* linked list of all blocks */
    uint32_t         obj_size;      /* size of each object (user-requested) */
    uint32_t         obj_size_aligned; /* size aligned to R8E_ALLOC_ALIGN */
    uint32_t         total_allocs;  /* total allocations */
    uint32_t         total_frees;   /* total frees */
    uint32_t         current_count; /* currently allocated objects */
    uint32_t         peak_count;    /* peak allocated objects */
    uint32_t         block_count;   /* number of blocks */
} R8ESlab;

/*
 * Initialize a slab allocator for a given object size.
 * Does NOT allocate any blocks; first allocation triggers block creation.
 */
void r8e_slab_init(R8ESlab *slab, uint32_t obj_size) {
    if (!slab) return;
    memset(slab, 0, sizeof(R8ESlab));

    /* Ensure minimum size can hold a free list pointer */
    if (obj_size < sizeof(R8ESlabFreeNode)) {
        obj_size = sizeof(R8ESlabFreeNode);
    }
    slab->obj_size = obj_size;
    slab->obj_size_aligned = (uint32_t)align_up(obj_size, R8E_ALLOC_ALIGN);
}

/*
 * Allocate a new block of slab memory and add all slots to the free list.
 */
static bool slab_grow(R8ESlab *slab) {
    uint32_t count = R8E_SLAB_BLOCK_COUNT;
    size_t data_size = (size_t)slab->obj_size_aligned * count;

    R8ESlabBlock *block = (R8ESlabBlock *)malloc(sizeof(R8ESlabBlock));
    if (!block) return false;

    block->data = (uint8_t *)malloc(data_size);
    if (!block->data) {
        free(block);
        return false;
    }

    /* Zero the block (security) */
    memset(block->data, 0, data_size);

    block->capacity = count;
    block->obj_size = slab->obj_size_aligned;
    block->next_block = slab->blocks;
    slab->blocks = block;
    slab->block_count++;

    /* Build free list from the new block (link all slots) */
    for (uint32_t i = 0; i < count; i++) {
        R8ESlabFreeNode *node =
            (R8ESlabFreeNode *)(block->data + (size_t)i * slab->obj_size_aligned);
        node->next = slab->free_list;
        slab->free_list = node;
    }

    return true;
}

/*
 * Allocate one object from the slab.
 * O(1) when free list is non-empty.
 *
 * @return  Pointer to zeroed memory of slab->obj_size bytes, or NULL.
 */
void *r8e_slab_alloc(R8ESlab *slab) {
    if (!slab) return NULL;

    /* Grow if free list is empty */
    if (!slab->free_list) {
        if (!slab_grow(slab)) return NULL;
    }

    /* Pop from free list */
    R8ESlabFreeNode *node = slab->free_list;
    slab->free_list = node->next;

    /* Zero the returned memory */
    memset(node, 0, slab->obj_size_aligned);

    slab->total_allocs++;
    slab->current_count++;
    if (slab->current_count > slab->peak_count) {
        slab->peak_count = slab->current_count;
    }

    return (void *)node;
}

/*
 * Return an object to the slab's free list.
 * O(1) operation.
 *
 * WARNING: Caller must ensure ptr was allocated from this slab.
 *          Double-free is undefined behavior.
 */
void r8e_slab_free(R8ESlab *slab, void *ptr) {
    if (!slab || !ptr) return;

#ifndef NDEBUG
    /* Debug: scrub memory to catch use-after-free */
    memset(ptr, 0xCD, slab->obj_size_aligned);
#endif

    /* Push onto free list */
    R8ESlabFreeNode *node = (R8ESlabFreeNode *)ptr;
    node->next = slab->free_list;
    slab->free_list = node;

    slab->total_frees++;
    slab->current_count--;
}

/*
 * Check if a pointer was allocated from this slab.
 * O(n) in number of blocks; use only for debugging.
 */
bool r8e_slab_contains(const R8ESlab *slab, const void *ptr) {
    if (!slab || !ptr) return false;
    const uint8_t *p = (const uint8_t *)ptr;
    for (const R8ESlabBlock *block = slab->blocks; block;
         block = block->next_block) {
        size_t block_size = (size_t)block->obj_size * block->capacity;
        if (p >= block->data && p < block->data + block_size) {
            /* Check alignment */
            size_t offset = (size_t)(p - block->data);
            if (offset % block->obj_size == 0) return true;
        }
    }
    return false;
}

/*
 * Destroy the slab, freeing all blocks.
 * All previously allocated objects become invalid.
 */
void r8e_slab_destroy(R8ESlab *slab) {
    if (!slab) return;

    R8ESlabBlock *block = slab->blocks;
    while (block) {
        R8ESlabBlock *next = block->next_block;
        free(block->data);
        free(block);
        block = next;
    }

    memset(slab, 0, sizeof(R8ESlab));
}

/*
 * Query slab statistics.
 */
uint32_t r8e_slab_current_count(const R8ESlab *slab) {
    return slab ? slab->current_count : 0;
}

uint32_t r8e_slab_peak_count(const R8ESlab *slab) {
    return slab ? slab->peak_count : 0;
}

uint32_t r8e_slab_obj_size(const R8ESlab *slab) {
    return slab ? slab->obj_size : 0;
}

/* =========================================================================
 * SLAB POOL - Pre-configured slabs for common object sizes
 *
 * Provides a single "pool" that routes allocations to the right slab
 * based on requested size.
 * ========================================================================= */

typedef struct R8ESlabPool {
    R8ESlab slabs[R8E_SLAB_NUM_CLASSES];
    uint32_t sizes[R8E_SLAB_NUM_CLASSES]; /* sorted size thresholds */
} R8ESlabPool;

/*
 * Initialize the slab pool with standard object tier sizes.
 */
void r8e_slab_pool_init(R8ESlabPool *pool) {
    if (!pool) return;
    memset(pool, 0, sizeof(R8ESlabPool));

    pool->sizes[0] = R8E_SLAB_SIZE_TIER0;    /* 24 bytes */
    pool->sizes[1] = R8E_SLAB_SIZE_TIER1_SM;  /* 40 bytes */
    pool->sizes[2] = R8E_SLAB_SIZE_TIER1_LG;  /* 72 bytes */

    for (int i = 0; i < R8E_SLAB_NUM_CLASSES; i++) {
        r8e_slab_init(&pool->slabs[i], pool->sizes[i]);
    }
}

/*
 * Allocate from the slab pool, choosing the smallest slab that fits.
 * Returns NULL if size exceeds all slab classes (caller should use
 * arena or malloc for oversized objects).
 */
void *r8e_slab_pool_alloc(R8ESlabPool *pool, uint32_t size) {
    if (!pool) return NULL;

    for (int i = 0; i < R8E_SLAB_NUM_CLASSES; i++) {
        if (size <= pool->sizes[i]) {
            return r8e_slab_alloc(&pool->slabs[i]);
        }
    }

    /* Size exceeds all slab classes */
    return NULL;
}

/*
 * Free an object back to the slab pool.
 * Caller must provide the original allocation size to route to the
 * correct slab.
 */
void r8e_slab_pool_free(R8ESlabPool *pool, void *ptr, uint32_t size) {
    if (!pool || !ptr) return;

    for (int i = 0; i < R8E_SLAB_NUM_CLASSES; i++) {
        if (size <= pool->sizes[i]) {
            r8e_slab_free(&pool->slabs[i], ptr);
            return;
        }
    }

    /* Should not reach here if caller uses correct size */
}

/*
 * Destroy all slabs in the pool.
 */
void r8e_slab_pool_destroy(R8ESlabPool *pool) {
    if (!pool) return;

    for (int i = 0; i < R8E_SLAB_NUM_CLASSES; i++) {
        r8e_slab_destroy(&pool->slabs[i]);
    }
}

/* =========================================================================
 * MEMORY TRACKER
 *
 * Global (or per-realm) memory tracking for enforcing limits.
 * Tracks current allocation, peak allocation, and allocation count.
 * ========================================================================= */

typedef struct R8EMemTracker {
    size_t   current;      /* current bytes allocated */
    size_t   peak;         /* peak bytes allocated */
    size_t   limit;        /* maximum bytes allowed (0 = unlimited) */
    uint64_t total_allocs; /* lifetime allocation count */
    uint64_t total_frees;  /* lifetime free count */
    uint64_t total_bytes;  /* lifetime bytes allocated */
} R8EMemTracker;

void r8e_mem_tracker_init(R8EMemTracker *tracker, size_t limit) {
    if (!tracker) return;
    memset(tracker, 0, sizeof(R8EMemTracker));
    tracker->limit = limit;
}

/*
 * Record an allocation. Returns false if limit would be exceeded.
 */
bool r8e_mem_tracker_alloc(R8EMemTracker *tracker, size_t size) {
    if (!tracker) return true;
    if (tracker->limit > 0 && tracker->current + size > tracker->limit) {
        return false; /* would exceed limit */
    }
    tracker->current += size;
    tracker->total_bytes += size;
    tracker->total_allocs++;
    if (tracker->current > tracker->peak) {
        tracker->peak = tracker->current;
    }
    return true;
}

/*
 * Record a deallocation.
 */
void r8e_mem_tracker_free(R8EMemTracker *tracker, size_t size) {
    if (!tracker) return;
    if (size > tracker->current) {
        /* Underflow: should not happen, but clamp to zero */
        tracker->current = 0;
    } else {
        tracker->current -= size;
    }
    tracker->total_frees++;
}

size_t r8e_mem_tracker_current(const R8EMemTracker *tracker) {
    return tracker ? tracker->current : 0;
}

size_t r8e_mem_tracker_peak(const R8EMemTracker *tracker) {
    return tracker ? tracker->peak : 0;
}

/* =========================================================================
 * TRACKED MALLOC/FREE
 *
 * Wrappers around malloc/free that integrate with the memory tracker.
 * Used for allocations that don't fit in arenas or slabs.
 * ========================================================================= */

/*
 * Tracked allocation: allocates (size + 8) bytes, stores size in the
 * first 8 bytes, returns pointer past the header. This allows r8e_tracked_free
 * to know how much to decrement from the tracker.
 */
void *r8e_tracked_alloc(R8EMemTracker *tracker, size_t size) {
    if (size == 0) return NULL;

    size_t total = size + sizeof(size_t);
    if (tracker && !r8e_mem_tracker_alloc(tracker, total)) {
        return NULL; /* limit exceeded */
    }

    uint8_t *raw = (uint8_t *)malloc(total);
    if (!raw) {
        if (tracker) r8e_mem_tracker_free(tracker, total);
        return NULL;
    }

    /* Store size in header */
    memcpy(raw, &total, sizeof(size_t));
    /* Zero the user portion */
    memset(raw + sizeof(size_t), 0, size);

    return raw + sizeof(size_t);
}

void r8e_tracked_free(R8EMemTracker *tracker, void *ptr) {
    if (!ptr) return;

    uint8_t *raw = (uint8_t *)ptr - sizeof(size_t);
    size_t total;
    memcpy(&total, raw, sizeof(size_t));

    if (tracker) r8e_mem_tracker_free(tracker, total);

    /* Scrub memory in debug mode */
#ifndef NDEBUG
    memset(raw, 0xDD, total);
#endif

    free(raw);
}

void *r8e_tracked_realloc(R8EMemTracker *tracker, void *ptr, size_t new_size) {
    if (!ptr) return r8e_tracked_alloc(tracker, new_size);
    if (new_size == 0) {
        r8e_tracked_free(tracker, ptr);
        return NULL;
    }

    uint8_t *old_raw = (uint8_t *)ptr - sizeof(size_t);
    size_t old_total;
    memcpy(&old_total, old_raw, sizeof(size_t));
    size_t old_user = old_total - sizeof(size_t);

    size_t new_total = new_size + sizeof(size_t);

    /* Check limit for the growth */
    if (tracker && new_total > old_total) {
        size_t growth = new_total - old_total;
        if (!r8e_mem_tracker_alloc(tracker, growth)) {
            return NULL;
        }
    }

    uint8_t *new_raw = (uint8_t *)realloc(old_raw, new_total);
    if (!new_raw) {
        /* Undo tracker adjustment */
        if (tracker && new_total > old_total) {
            r8e_mem_tracker_free(tracker, new_total - old_total);
        }
        return NULL;
    }

    /* Store new size */
    memcpy(new_raw, &new_total, sizeof(size_t));

    /* Zero newly allocated portion */
    if (new_size > old_user) {
        memset(new_raw + sizeof(size_t) + old_user, 0, new_size - old_user);
    }

    /* Update tracker for shrinkage */
    if (tracker && new_total < old_total) {
        r8e_mem_tracker_free(tracker, old_total - new_total);
    }

    return new_raw + sizeof(size_t);
}

/* =========================================================================
 * STACK CANARY SUPPORT (Section 11.4)
 *
 * Place canary values at bytecode frame boundaries to detect stack
 * buffer overwrites.
 * ========================================================================= */

void r8e_canary_write(void *ptr) {
    if (ptr) {
        uint64_t canary = R8E_CANARY_VALUE;
        memcpy(ptr, &canary, sizeof(uint64_t));
    }
}

bool r8e_canary_check(const void *ptr) {
    if (!ptr) return false;
    uint64_t val;
    memcpy(&val, ptr, sizeof(uint64_t));
    return val == R8E_CANARY_VALUE;
}

/* =========================================================================
 * NaN-BOX VALIDATION (Section 11.4)
 *
 * Validate NaN-box tag bits at trust boundaries.
 * Returns true if the value has a valid encoding.
 * ========================================================================= */

bool r8e_value_validate(R8EValue v) {
    /* Doubles: anything below the tag boundary */
    if (v < 0xFFF8000000000000ULL) return true;

    uint16_t tag = (uint16_t)(v >> 48);

    switch (tag) {
    case 0xFFF8: /* Int32 */
        return true;

    case 0xFFF9: /* Pointer */
        /* Pointer must be non-zero and properly aligned */
        {
            uintptr_t ptr = (uintptr_t)(v & 0x0000FFFFFFFFFFFFULL);
            return (ptr != 0) && ((ptr & 0x7) == 0 || (ptr & 0x3) == 0);
            /* Allow 4-byte or 8-byte alignment */
        }

    case 0xFFFA: /* Special values */
        {
            uint64_t payload = v & 0x0000FFFFFFFFFFFFULL;
            return payload <= 3; /* undefined, null, true, false */
        }

    case 0xFFFB: /* Symbol */
        return true;

    case 0xFFFC: /* Atom */
        return true;

    case 0xFFFD: /* Inline string */
        {
            int len = (int)((v >> 45) & 0x7);
            return len <= 7;
        }

    default:
        /* Tags 0xFFFE and 0xFFFF are reserved/invalid */
        return false;
    }
}

/* =========================================================================
 * ARENA POOL (for multi-realm support)
 *
 * Manages a set of arenas, one per realm, with aggregate tracking.
 * ========================================================================= */

#define R8E_MAX_ARENAS 16

typedef struct R8EArenaPool {
    R8EArena       *arenas[R8E_MAX_ARENAS];
    uint8_t         count;
    R8EMemTracker   tracker;
} R8EArenaPool;

void r8e_arena_pool_init(R8EArenaPool *pool, size_t total_limit) {
    if (!pool) return;
    memset(pool, 0, sizeof(R8EArenaPool));
    r8e_mem_tracker_init(&pool->tracker, total_limit);
}

/*
 * Add a new arena to the pool.
 * Returns the arena index (0-15), or -1 on failure.
 */
int r8e_arena_pool_add(R8EArenaPool *pool, size_t size) {
    if (!pool || pool->count >= R8E_MAX_ARENAS) return -1;

    if (!r8e_mem_tracker_alloc(&pool->tracker, size)) {
        return -1; /* aggregate limit exceeded */
    }

    R8EArena *arena = r8e_arena_create(size);
    if (!arena) {
        r8e_mem_tracker_free(&pool->tracker, size);
        return -1;
    }

    int idx = pool->count;
    pool->arenas[idx] = arena;
    pool->count++;
    return idx;
}

R8EArena *r8e_arena_pool_get(R8EArenaPool *pool, int index) {
    if (!pool || index < 0 || index >= pool->count) return NULL;
    return pool->arenas[index];
}

void r8e_arena_pool_destroy(R8EArenaPool *pool) {
    if (!pool) return;
    for (int i = 0; i < pool->count; i++) {
        if (pool->arenas[i]) {
            r8e_arena_destroy(pool->arenas[i]);
            pool->arenas[i] = NULL;
        }
    }
    pool->count = 0;
}
