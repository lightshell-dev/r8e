/*
 * r8e_atom.c - Atom Table with Bloom Filter for String Interning
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 4.5 for design rationale.
 *
 * String interning: property names are stored once and referenced by a
 * 32-bit atom index. Comparing two interned strings reduces to comparing
 * two integers. The atom table uses a hash table with separate chaining
 * and a 256-bit Bloom filter for fast "definitely not interned" checks.
 *
 * Pre-populated with ~256 common names (length, prototype, constructor, etc.)
 * and all JavaScript keywords. The Bloom filter avoids hash table lookups
 * for strings that are definitely not interned, which is common during
 * parsing of new identifiers.
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Type definitions (self-contained until shared headers are integrated)
 * ========================================================================= */

typedef uint64_t R8EValue;

/* Forward declarations */
typedef struct R8EContext R8EContext;

struct R8EContext {
    void *arena;  /* placeholder */
};

/* =========================================================================
 * Memory allocation helpers (mirrors r8e_string.c)
 * ========================================================================= */

static inline void *r8e_alloc(R8EContext *ctx, size_t size) {
    (void)ctx;
    return malloc(size);
}

static inline void r8e_free(R8EContext *ctx, void *ptr) {
    (void)ctx;
    free(ptr);
}

/* =========================================================================
 * FNV-1a Hash (same as r8e_string.c for consistency)
 * ========================================================================= */

#define R8E_FNV_OFFSET_BASIS 0x811C9DC5u
#define R8E_FNV_PRIME        0x01000193u

static uint32_t r8e_fnv1a(const char *data, uint32_t len) {
    uint32_t hash = R8E_FNV_OFFSET_BASIS;
    const uint8_t *p = (const uint8_t *)data;
    for (uint32_t i = 0; i < len; i++) {
        hash ^= p[i];
        hash *= R8E_FNV_PRIME;
    }
    return hash;
}

/* =========================================================================
 * R8EAtomEntry - Single entry in the atom hash table
 *
 * Each entry stores a copy of the interned string (not a pointer to an
 * R8EString, because atoms are simpler: they are always ASCII property
 * names or keywords, and we need them to be self-contained).
 * ========================================================================= */

typedef struct R8EAtomEntry {
    uint32_t             hash;       /* FNV-1a hash of the string */
    uint32_t             atom_id;    /* unique atom index (1-based, 0 = none) */
    uint32_t             length;     /* byte length of the string */
    char                *str;        /* heap-allocated copy of the string */
    struct R8EAtomEntry *next;       /* chain pointer for collision resolution */
} R8EAtomEntry;

/* =========================================================================
 * R8EAtomTable - Main atom table structure (Section 4.5)
 *
 * 256-bit Bloom filter (4 x uint64_t) for fast negative lookups.
 * Hash table with separate chaining for actual storage.
 * Atom IDs are 1-based; 0 means "not an atom" / "not found".
 * ========================================================================= */

#define R8E_ATOM_TABLE_INITIAL_CAP 512  /* must be power of 2 */
#define R8E_ATOM_BLOOM_BITS        256  /* 4 * 64 bits */

typedef struct R8EAtomTable {
    uint64_t      bloom[4];     /* 256-bit Bloom filter */
    uint32_t      count;        /* number of interned atoms */
    uint32_t      capacity;     /* bucket count (power of 2) */
    uint32_t      next_id;      /* next atom ID to assign */
    R8EAtomEntry **buckets;     /* hash table buckets */

    /*
     * Flat lookup array: atoms_by_id[atom_id] -> R8EAtomEntry*
     * Allows O(1) lookup by atom ID (for r8e_atom_get).
     */
    R8EAtomEntry **atoms_by_id;
    uint32_t       id_capacity; /* allocated size of atoms_by_id */
} R8EAtomTable;

/*
 * Global atom table. In the full engine this will be per-context or
 * per-realm (shared immutable). For now, one global instance.
 */
static R8EAtomTable *g_atom_table = NULL;

/* =========================================================================
 * Bloom Filter Operations
 *
 * 256-bit Bloom filter using 3 independent hash functions derived from
 * the FNV-1a hash. Each function maps to a bit position in [0, 255].
 *
 * False positive rate at 256 entries: ~18% with k=3, m=256.
 * At typical workloads (~500-1000 atoms), the filter still eliminates
 * a large fraction of non-interned lookups before touching the hash table.
 * ========================================================================= */

static inline void r8e_bloom_set(uint64_t *bloom, uint32_t hash) {
    /* Derive 3 bit positions from the hash */
    uint32_t h1 = hash & 0xFF;           /* bits [7:0] */
    uint32_t h2 = (hash >> 8) & 0xFF;    /* bits [15:8] */
    uint32_t h3 = (hash >> 16) & 0xFF;   /* bits [23:16] */

    bloom[h1 >> 6] |= (1ULL << (h1 & 63));
    bloom[h2 >> 6] |= (1ULL << (h2 & 63));
    bloom[h3 >> 6] |= (1ULL << (h3 & 63));
}

static inline bool r8e_bloom_maybe_has(const uint64_t *bloom, uint32_t hash) {
    uint32_t h1 = hash & 0xFF;
    uint32_t h2 = (hash >> 8) & 0xFF;
    uint32_t h3 = (hash >> 16) & 0xFF;

    if (!(bloom[h1 >> 6] & (1ULL << (h1 & 63)))) return false;
    if (!(bloom[h2 >> 6] & (1ULL << (h2 & 63)))) return false;
    if (!(bloom[h3 >> 6] & (1ULL << (h3 & 63)))) return false;
    return true;  /* maybe present - must confirm in hash table */
}

/* Forward declaration for r8e_atom_table_init error path */
void r8e_atom_table_destroy(R8EContext *ctx);

/* =========================================================================
 * Atom Table Internal Operations
 * ========================================================================= */

/**
 * Grow the atoms_by_id lookup array if needed.
 */
static bool r8e_atom_grow_id_array(R8EContext *ctx, R8EAtomTable *table) {
    if (table->next_id < table->id_capacity) return true;

    uint32_t new_cap = table->id_capacity * 2;
    if (new_cap < 64) new_cap = 64;

    R8EAtomEntry **new_arr = (R8EAtomEntry **)r8e_alloc(ctx,
        sizeof(R8EAtomEntry *) * new_cap);
    if (!new_arr) return false;

    memset(new_arr, 0, sizeof(R8EAtomEntry *) * new_cap);
    if (table->atoms_by_id) {
        memcpy(new_arr, table->atoms_by_id,
               sizeof(R8EAtomEntry *) * table->id_capacity);
        r8e_free(ctx, table->atoms_by_id);
    }
    table->atoms_by_id = new_arr;
    table->id_capacity = new_cap;
    return true;
}

/**
 * Rehash the table when load factor exceeds 0.75.
 */
static bool r8e_atom_table_rehash(R8EContext *ctx, R8EAtomTable *table) {
    uint32_t new_cap = table->capacity * 2;
    R8EAtomEntry **new_buckets = (R8EAtomEntry **)r8e_alloc(ctx,
        sizeof(R8EAtomEntry *) * new_cap);
    if (!new_buckets) return false;

    memset(new_buckets, 0, sizeof(R8EAtomEntry *) * new_cap);

    /* Re-insert all entries */
    uint32_t mask = new_cap - 1;
    for (uint32_t i = 0; i < table->capacity; i++) {
        R8EAtomEntry *entry = table->buckets[i];
        while (entry) {
            R8EAtomEntry *next = entry->next;
            uint32_t slot = entry->hash & mask;
            entry->next = new_buckets[slot];
            new_buckets[slot] = entry;
            entry = next;
        }
    }

    r8e_free(ctx, table->buckets);
    table->buckets = new_buckets;
    table->capacity = new_cap;
    return true;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * Intern a string in the atom table.
 *
 * If the string is already interned, returns the existing atom ID.
 * Otherwise, creates a new entry with a new atom ID.
 *
 * @param ctx  engine context
 * @param str  string data (need not be null-terminated)
 * @param len  byte length
 * @return     atom ID (1-based), or 0 on error
 */
uint32_t r8e_atom_intern(R8EContext *ctx, const char *str, uint32_t len) {
    R8EAtomTable *table = g_atom_table;
    if (!table) return 0;

    uint32_t hash = r8e_fnv1a(str, len);

    /* Check hash table for existing entry */
    uint32_t slot = hash & (table->capacity - 1);
    R8EAtomEntry *entry = table->buckets[slot];
    while (entry) {
        if (entry->hash == hash && entry->length == len &&
            memcmp(entry->str, str, len) == 0) {
            return entry->atom_id;
        }
        entry = entry->next;
    }

    /* Not found: create new entry */
    if (table->count * 4 >= table->capacity * 3) {
        /* Load factor > 0.75: rehash */
        if (!r8e_atom_table_rehash(ctx, table)) return 0;
        /* Recompute slot after rehash */
        slot = hash & (table->capacity - 1);
    }

    if (!r8e_atom_grow_id_array(ctx, table)) return 0;

    R8EAtomEntry *new_entry = (R8EAtomEntry *)r8e_alloc(ctx,
        sizeof(R8EAtomEntry));
    if (!new_entry) return 0;

    new_entry->str = (char *)r8e_alloc(ctx, len + 1);
    if (!new_entry->str) {
        r8e_free(ctx, new_entry);
        return 0;
    }
    memcpy(new_entry->str, str, len);
    new_entry->str[len] = '\0';

    new_entry->hash = hash;
    new_entry->length = len;
    new_entry->atom_id = table->next_id;

    /* Insert at head of chain */
    new_entry->next = table->buckets[slot];
    table->buckets[slot] = new_entry;

    /* Register in ID lookup array */
    table->atoms_by_id[table->next_id] = new_entry;
    table->next_id++;
    table->count++;

    /* Update Bloom filter */
    r8e_bloom_set(table->bloom, hash);

    return new_entry->atom_id;
}

/**
 * Look up an atom's string by its ID.
 *
 * @param ctx      engine context
 * @param atom_id  atom ID (1-based)
 * @return         null-terminated string, or NULL if invalid ID
 */
const char *r8e_atom_get(R8EContext *ctx, uint32_t atom_id) {
    (void)ctx;
    R8EAtomTable *table = g_atom_table;
    if (!table || atom_id == 0 || atom_id >= table->next_id)
        return NULL;
    R8EAtomEntry *entry = table->atoms_by_id[atom_id];
    return entry ? entry->str : NULL;
}

/**
 * Look up whether a string is already interned, without interning it.
 * Uses the Bloom filter for a fast "definitely not present" check.
 *
 * @param ctx  engine context
 * @param str  string data
 * @param len  byte length
 * @return     atom ID if found, or 0 if not interned
 */
uint32_t r8e_atom_lookup(R8EContext *ctx, const char *str, uint32_t len) {
    (void)ctx;
    R8EAtomTable *table = g_atom_table;
    if (!table) return 0;

    uint32_t hash = r8e_fnv1a(str, len);

    /* Bloom filter: fast negative check */
    if (!r8e_bloom_maybe_has(table->bloom, hash))
        return 0;

    /* Bloom says "maybe": check hash table */
    uint32_t slot = hash & (table->capacity - 1);
    R8EAtomEntry *entry = table->buckets[slot];
    while (entry) {
        if (entry->hash == hash && entry->length == len &&
            memcmp(entry->str, str, len) == 0) {
            return entry->atom_id;
        }
        entry = entry->next;
    }
    return 0;
}

/**
 * Get the byte length of an atom's string.
 */
uint32_t r8e_atom_length(R8EContext *ctx, uint32_t atom_id) {
    (void)ctx;
    R8EAtomTable *table = g_atom_table;
    if (!table || atom_id == 0 || atom_id >= table->next_id)
        return 0;
    R8EAtomEntry *entry = table->atoms_by_id[atom_id];
    return entry ? entry->length : 0;
}

/**
 * Get the precomputed hash of an atom.
 */
uint32_t r8e_atom_hash(R8EContext *ctx, uint32_t atom_id) {
    (void)ctx;
    R8EAtomTable *table = g_atom_table;
    if (!table || atom_id == 0 || atom_id >= table->next_id)
        return 0;
    R8EAtomEntry *entry = table->atoms_by_id[atom_id];
    return entry ? entry->hash : 0;
}

/**
 * Compare two atoms for equality. Since atoms are interned,
 * this is a simple integer comparison.
 *
 * Note: This will move to a shared header once r8e_atoms.h is created.
 * Defined here for now so callers can use it.
 */
bool r8e_atom_equal(uint32_t a, uint32_t b) {
    return a == b;
}

/* =========================================================================
 * Pre-populated Atoms (~256 common names)
 *
 * These are interned at table initialization time. The atom IDs for these
 * are deterministic and can be used as compile-time constants once the
 * shared header is generated.
 *
 * Organization:
 *   - Object/prototype properties
 *   - Primitive names and values
 *   - Built-in constructor names
 *   - Error types
 *   - Math/JSON/Date/RegExp
 *   - Collection types
 *   - All ES2023 keywords
 *   - Property descriptor keys
 *   - Function-related names
 *   - Iterator/generator protocol
 *   - Promise protocol
 *   - Common property names
 *   - Module-related
 *   - Miscellaneous
 * ========================================================================= */

static const char *r8e_builtin_atoms[] = {
    /* --- Object / prototype properties (1-20) --- */
    "length",            /*  1 */
    "prototype",         /*  2 */
    "constructor",       /*  3 */
    "toString",          /*  4 */
    "valueOf",           /*  5 */
    "hasOwnProperty",    /*  6 */
    "__proto__",         /*  7 */
    "isPrototypeOf",     /*  8 */
    "propertyIsEnumerable", /* 9 */
    "toLocaleString",    /* 10 */
    "defineProperty",    /* 11 */
    "defineProperties",  /* 12 */
    "getOwnPropertyDescriptor", /* 13 */
    "getOwnPropertyNames", /* 14 */
    "getPrototypeOf",    /* 15 */
    "setPrototypeOf",    /* 16 */
    "keys",              /* 17 */
    "values",            /* 18 */
    "entries",           /* 19 */
    "assign",            /* 20 */

    /* --- Primitive type names and special values (21-30) --- */
    "undefined",         /* 21 */
    "null",              /* 22 */
    "true",              /* 23 */
    "false",             /* 24 */
    "NaN",               /* 25 */
    "Infinity",          /* 26 */
    "number",            /* 27 */
    "string",            /* 28 */
    "boolean",           /* 29 */
    "object",            /* 30 */

    /* --- Built-in constructor names (31-48) --- */
    "Object",            /* 31 */
    "Array",             /* 32 */
    "String",            /* 33 */
    "Number",            /* 34 */
    "Boolean",           /* 35 */
    "Function",          /* 36 */
    "Symbol",            /* 37 */
    "BigInt",            /* 38 */
    "Date",              /* 39 */
    "RegExp",            /* 40 */
    "Error",             /* 41 */
    "Math",              /* 42 */
    "JSON",              /* 43 */
    "ArrayBuffer",       /* 44 */
    "DataView",          /* 45 */
    "Int8Array",         /* 46 */
    "Uint8Array",        /* 47 */
    "Int16Array",        /* 48 */

    /* --- More typed arrays and collections (49-66) --- */
    "Uint16Array",       /* 49 */
    "Int32Array",        /* 50 */
    "Uint32Array",       /* 51 */
    "Float32Array",      /* 52 */
    "Float64Array",      /* 53 */
    "Uint8ClampedArray", /* 54 */
    "BigInt64Array",     /* 55 */
    "BigUint64Array",    /* 56 */
    "Map",               /* 57 */
    "Set",               /* 58 */
    "WeakMap",           /* 59 */
    "WeakSet",           /* 60 */
    "WeakRef",           /* 61 */
    "FinalizationRegistry", /* 62 */
    "Promise",           /* 63 */
    "Proxy",             /* 64 */
    "Reflect",           /* 65 */
    "Generator",         /* 66 */

    /* --- Error types (67-75) --- */
    "TypeError",         /* 67 */
    "RangeError",        /* 68 */
    "ReferenceError",    /* 69 */
    "SyntaxError",       /* 70 */
    "URIError",          /* 71 */
    "EvalError",         /* 72 */
    "AggregateError",    /* 73 */
    "InternalError",     /* 74 */
    "CompileError",      /* 75 */

    /* --- Error/function properties (76-88) --- */
    "name",              /* 76 */
    "message",           /* 77 */
    "stack",             /* 78 */
    "cause",             /* 79 */
    "errors",            /* 80 */
    "caller",            /* 81 */
    "callee",            /* 82 */
    "arguments",         /* 83 */
    "apply",             /* 84 */
    "call",              /* 85 */
    "bind",              /* 86 */
    "value",             /* 87 */
    "done",              /* 88 */

    /* --- Iterator / generator protocol (89-96) --- */
    "next",              /* 89 */
    "return",            /* 90 */
    "throw",             /* 91 */
    "iterator",          /* 92 */
    "asyncIterator",     /* 93 */
    "toStringTag",       /* 94 */
    "hasInstance",        /* 95 */
    "species",           /* 96 */

    /* --- Promise protocol (97-103) --- */
    "then",              /* 97 */
    "catch",             /* 98 */
    "finally",           /* 99 */
    "resolve",           /* 100 */
    "reject",            /* 101 */
    "all",               /* 102 */
    "race",              /* 103 */

    /* --- Property descriptor keys (104-110) --- */
    "get",               /* 104 */
    "set",               /* 105 */
    "configurable",      /* 106 */
    "enumerable",        /* 107 */
    "writable",          /* 108 */
    "symbol",            /* 109 */
    "function",          /* 110 */

    /* --- Keywords: declarations (111-119) --- */
    "var",               /* 111 */
    "let",               /* 112 */
    "const",             /* 113 */
    "class",             /* 114 */
    "extends",           /* 115 */
    "super",             /* 116 */
    "this",              /* 117 */
    "new",               /* 118 */
    "delete",            /* 119 */

    /* --- Keywords: control flow (120-136) --- */
    "if",                /* 120 */
    "else",              /* 121 */
    "for",               /* 122 */
    "while",             /* 123 */
    "do",                /* 124 */
    "break",             /* 125 */
    "continue",          /* 126 */
    "switch",            /* 127 */
    "case",              /* 128 */
    "default",           /* 129 */
    "try",               /* 130 */
    /* "catch" already at 98 */
    /* "finally" already at 99 */
    /* "throw" already at 91 */
    /* "return" already at 90 */
    "with",              /* 131 */
    "yield",             /* 132 */
    "await",             /* 133 */
    "async",             /* 134 */
    "of",                /* 135 */
    "in",                /* 136 */

    /* --- Keywords: operators and literals (137-146) --- */
    "typeof",            /* 137 */
    "instanceof",        /* 138 */
    "void",              /* 139 */
    "debugger",          /* 140 */
    "export",            /* 141 */
    "import",            /* 142 */
    "from",              /* 143 */
    "as",                /* 144 */
    "static",            /* 145 */
    "implements",        /* 146 */

    /* --- Reserved words / strict mode (147-154) --- */
    "interface",         /* 147 */
    "package",           /* 148 */
    "private",           /* 149 */
    "protected",         /* 150 */
    "public",            /* 151 */
    "enum",              /* 152 */
    "eval",              /* 153 */
    "target",            /* 154 */

    /* --- Array methods (155-172) --- */
    "push",              /* 155 */
    "pop",               /* 156 */
    "shift",             /* 157 */
    "unshift",           /* 158 */
    "slice",             /* 159 */
    "splice",            /* 160 */
    "concat",            /* 161 */
    "join",              /* 162 */
    "reverse",           /* 163 */
    "sort",              /* 164 */
    "indexOf",           /* 165 */
    "lastIndexOf",       /* 166 */
    "includes",          /* 167 */
    "find",              /* 168 */
    "findIndex",         /* 169 */
    "findLast",          /* 170 */
    "findLastIndex",     /* 171 */
    "every",             /* 172 */

    /* --- Array methods continued (173-188) --- */
    "some",              /* 173 */
    "forEach",           /* 174 */
    "map",               /* 175 */
    "filter",            /* 176 */
    "reduce",            /* 177 */
    "reduceRight",       /* 178 */
    "fill",              /* 179 */
    "copyWithin",        /* 180 */
    "flat",              /* 181 */
    "flatMap",           /* 182 */
    "at",                /* 183 */
    "isArray",           /* 184 */
    "from",              /* already interned, will deduplicate */
    "of",                /* already interned, will deduplicate */

    /* --- String methods (185-206) --- */
    "charAt",            /* 185 */
    "charCodeAt",        /* 186 */
    "codePointAt",       /* 187 */
    "startsWith",        /* 188 */
    "endsWith",          /* 189 */
    "repeat",            /* 190 */
    "padStart",          /* 191 */
    "padEnd",            /* 192 */
    "trim",              /* 193 */
    "trimStart",         /* 194 */
    "trimEnd",           /* 195 */
    "replace",           /* 196 */
    "replaceAll",        /* 197 */
    "split",             /* 198 */
    "substring",         /* 199 */
    "toLowerCase",       /* 200 */
    "toUpperCase",       /* 201 */
    "match",             /* 202 */
    "matchAll",          /* 203 */
    "search",            /* 204 */
    "normalize",         /* 205 */
    "raw",               /* 206 */

    /* --- Number / Math (207-226) --- */
    "toFixed",           /* 207 */
    "toPrecision",       /* 208 */
    "toExponential",     /* 209 */
    "isFinite",          /* 210 */
    "isNaN",             /* 211 */
    "isInteger",         /* 212 */
    "isSafeInteger",     /* 213 */
    "parseInt",          /* 214 */
    "parseFloat",        /* 215 */
    "MAX_SAFE_INTEGER",  /* 216 */
    "MIN_SAFE_INTEGER",  /* 217 */
    "EPSILON",           /* 218 */
    "MAX_VALUE",         /* 219 */
    "MIN_VALUE",         /* 220 */
    "POSITIVE_INFINITY", /* 221 */
    "NEGATIVE_INFINITY", /* 222 */
    "PI",                /* 223 */
    "E",                 /* 224 */
    "abs",               /* 225 */
    "floor",             /* 226 */

    /* --- Math methods continued (227-244) --- */
    "ceil",              /* 227 */
    "round",             /* 228 */
    "trunc",             /* 229 */
    "sqrt",              /* 230 */
    "cbrt",              /* 231 */
    "pow",               /* 232 */
    "log",               /* 233 */
    "log2",              /* 234 */
    "log10",             /* 235 */
    "exp",               /* 236 */
    "sin",               /* 237 */
    "cos",               /* 238 */
    "tan",               /* 239 */
    "min",               /* 240 */
    "max",               /* 241 */
    "random",            /* 242 */
    "sign",              /* 243 */
    "clz32",             /* 244 */

    /* --- Object static methods (245-252) --- */
    "freeze",            /* 245 */
    "isFrozen",          /* 246 */
    "seal",              /* 247 */
    "isSealed",          /* 248 */
    "preventExtensions", /* 249 */
    "isExtensible",      /* 250 */
    "create",            /* 251 */
    "is",                /* 252 */

    /* --- Proxy / Reflect traps (253-266) --- */
    "apply",             /* already interned */
    "construct",         /* 253 */
    "getOwnPropertyDescriptor", /* already interned */
    "defineProperty",    /* already interned */
    "has",               /* 254 */
    "deleteProperty",    /* 255 */
    "ownKeys",           /* 256 */
    "getPrototypeOf",    /* already interned */
    "setPrototypeOf",    /* already interned */
    "isExtensible",      /* already interned */
    "preventExtensions", /* already interned */

    /* --- JSON (267-268) --- */
    "parse",             /* 257 */
    "stringify",         /* 258 */

    /* --- Date (269-276) --- */
    "now",               /* 259 */
    "getTime",           /* 260 */
    "setTime",           /* 261 */
    "getFullYear",       /* 262 */
    "getMonth",          /* 263 */
    "getDate",           /* 264 */
    "getDay",            /* 265 */
    "getHours",          /* 266 */

    /* --- RegExp (277-280) --- */
    "test",              /* 267 */
    "exec",              /* 268 */
    "source",            /* 269 */
    "flags",             /* 270 */

    /* --- Promise additional (281-285) --- */
    "allSettled",        /* 271 */
    "any",               /* 272 */
    "status",            /* 273 */
    "reason",            /* 274 */
    "fulfilled",         /* 275 */

    /* --- WeakRef / FinalizationRegistry (286-288) --- */
    "deref",             /* 276 */
    "register",          /* 277 */
    "unregister",        /* 278 */

    /* --- Miscellaneous common names (289-300+) --- */
    "size",              /* 279 */
    "has",               /* already interned */
    "add",               /* 280 */
    "clear",             /* 281 */
    "description",       /* 282 */
    "global",            /* 283 */
    "globalThis",        /* 284 */
    "console",           /* 285 */
    "Symbol",            /* already interned */
    "toPrimitive",       /* 286 */
    "isConcatSpreadable", /* 287 */
    "unscopables",       /* 288 */
    "iterator",          /* already interned */
    "toJSON",            /* 289 */
    "index",             /* 290 */
    "input",             /* 291 */
    "groups",            /* 292 */
    "lastIndex",         /* 293 */
    "dotAll",            /* 294 */
    "sticky",            /* 295 */
    "unicode",           /* 296 */
    "multiline",         /* 297 */
    "ignoreCase",        /* 298 */

    /* --- Sentinel --- */
    NULL
};

/* =========================================================================
 * Table Initialization and Destruction
 * ========================================================================= */

/**
 * Initialize the atom table and pre-populate with built-in atoms.
 *
 * @param ctx  engine context
 * @return     true on success, false on OOM
 */
bool r8e_atom_table_init(R8EContext *ctx) {
    if (g_atom_table) return true; /* already initialized */

    R8EAtomTable *table = (R8EAtomTable *)r8e_alloc(ctx,
        sizeof(R8EAtomTable));
    if (!table) return false;

    memset(table, 0, sizeof(R8EAtomTable));
    table->capacity = R8E_ATOM_TABLE_INITIAL_CAP;
    table->next_id = 1;  /* atom IDs are 1-based */

    table->buckets = (R8EAtomEntry **)r8e_alloc(ctx,
        sizeof(R8EAtomEntry *) * table->capacity);
    if (!table->buckets) {
        r8e_free(ctx, table);
        return false;
    }
    memset(table->buckets, 0, sizeof(R8EAtomEntry *) * table->capacity);

    /* Initialize ID lookup array */
    table->id_capacity = 512;
    table->atoms_by_id = (R8EAtomEntry **)r8e_alloc(ctx,
        sizeof(R8EAtomEntry *) * table->id_capacity);
    if (!table->atoms_by_id) {
        r8e_free(ctx, table->buckets);
        r8e_free(ctx, table);
        return false;
    }
    memset(table->atoms_by_id, 0,
           sizeof(R8EAtomEntry *) * table->id_capacity);

    g_atom_table = table;

    /* Pre-populate with built-in atoms */
    for (int i = 0; r8e_builtin_atoms[i] != NULL; i++) {
        const char *s = r8e_builtin_atoms[i];
        uint32_t len = (uint32_t)strlen(s);
        uint32_t id = r8e_atom_intern(ctx, s, len);
        if (id == 0) {
            /* OOM during pre-population: clean up */
            r8e_atom_table_destroy(ctx);
            return false;
        }
    }

    return true;
}

/**
 * Destroy the atom table and free all memory.
 */
void r8e_atom_table_destroy(R8EContext *ctx) {
    R8EAtomTable *table = g_atom_table;
    if (!table) return;

    /* Free all entries */
    for (uint32_t i = 0; i < table->capacity; i++) {
        R8EAtomEntry *entry = table->buckets[i];
        while (entry) {
            R8EAtomEntry *next = entry->next;
            r8e_free(ctx, entry->str);
            r8e_free(ctx, entry);
            entry = next;
        }
    }

    r8e_free(ctx, table->buckets);
    r8e_free(ctx, table->atoms_by_id);
    r8e_free(ctx, table);
    g_atom_table = NULL;
}

/**
 * Get the current number of interned atoms.
 */
uint32_t r8e_atom_count(R8EContext *ctx) {
    (void)ctx;
    R8EAtomTable *table = g_atom_table;
    return table ? table->count : 0;
}

/**
 * Convenience: intern a null-terminated C string.
 */
uint32_t r8e_atom_intern_cstr(R8EContext *ctx, const char *cstr) {
    return r8e_atom_intern(ctx, cstr, (uint32_t)strlen(cstr));
}

/**
 * Convenience: look up a null-terminated C string.
 */
uint32_t r8e_atom_lookup_cstr(R8EContext *ctx, const char *cstr) {
    return r8e_atom_lookup(ctx, cstr, (uint32_t)strlen(cstr));
}
