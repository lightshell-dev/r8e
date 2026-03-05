# r8e Complete Technical Specification
## Version 1.0 - Data-Validated Architecture

**Status:** Ready for Implementation
**Target:** ~166KB binary, full ES2023, less than 50% QuickJS memory, five-layer security
**Language:** C11 (no C++, minimal deps, maximum portability)

---

# TABLE OF CONTENTS

1. System Overview and Binary Budget
2. Value Representation (NaN-Boxing)
3. Object Model (4-Tier CDOL)
4. String Engine (UTF-8 + Inline Shorts)
5. Single-Pass Compiler (No AST)
6. Bytecode Format and Superinstructions
7. Interpreter and Dispatch
8. Garbage Collection (RC + Epoch Cycles)
9. Closures and Scopes (PVCC)
10. Regular Expression Engine (Tiered)
11. Security Architecture (5 Layers)
12. UI Rendering Stack (NanoUI)
13. File Structure and Build System
14. Engineering Timeline
15. Test Strategy

---

# 1. SYSTEM OVERVIEW

## 1.1 What r8e Is

r8e is a JavaScript engine that takes JS source code and executes it. It replaces V8 (Chrome, ~10M LOC) or SpiderMonkey (Firefox, ~4M LOC) in a ~166KB binary. No JIT compiler. Pure bytecode interpreter. This is deliberate: JITs add 5-10MB binary, create attack surface, and only help long-running compute workloads. For UI apps, IoT, embedded, and sandboxes, an optimized interpreter suffices.

## 1.2 Binary Size Budget

| Component | LOC | Binary |
|---|---|---|
| Lexer + Parser + Compiler | 8,000 | 20 KB |
| Interpreter + Dispatch | 4,000 | 10 KB |
| Value System + Allocator | 2,500 | 6 KB |
| Object Model (4-tier CDOL) | 3,000 | 8 KB |
| String Engine (UTF-8) | 2,500 | 6 KB |
| Closures + Scopes | 1,500 | 4 KB |
| GC (RC + Epoch Cycles) | 2,000 | 5 KB |
| RegExp Engine (Tiered) | 3,000 | 8 KB |
| Built-in Objects + Methods | 15,000 | 30 KB |
| Module System (ESM) | 2,000 | 5 KB |
| Security (5-layer) | 6,300 | 16 KB |
| JS Engine Subtotal | ~50K | ~118 KB |
| NanoDOM | 2,000 | 5 KB |
| NanoStyle | 2,500 | 6 KB |
| NanoLayout (Flexbox) | 3,500 | 9 KB |
| NanoPaint + Events | 3,300 | 8 KB |
| UI Stack Subtotal | ~11K | ~28 KB |
| NanoVG/SW Rasterizer | 6,500 | ~16 KB |
| GRAND TOTAL | ~68K | ~166 KB |

## 1.3 Memory Targets (Validated)

For a medium 50KB JS app (200 objects, 500 strings, 100 closures):

| Component | QuickJS | r8e | Savings | Proof |
|---|---|---|---|---|
| Parse-time peak | 250 KB | 60 KB | 76% | EXP-07 |
| Object storage | 19.2 KB | 4.8 KB | 75% | EXP-01 |
| String storage | 48 KB | 28 KB | 42% | EXP-05 |
| Closure storage | 7.2 KB | 2.8 KB | 61% | EXP-04 |
| Bytecode | 35 KB | 19.6 KB | 44% | EXP-06 |
| GC metadata | 15 KB | 4 KB | 73% | EXP-03 |
| Runtime total | ~136 KB | ~59 KB | 57% | |
| Peak (parse) | ~386 KB | ~119 KB | 69% | |

---

# 2. VALUE REPRESENTATION (NaN-Boxing)

## 2.1 The Problem

Every JS value (number, string, object, boolean, null, undefined) must be stored in memory. The naive approach (type tag + union) costs 16 bytes per value. With millions of values, this adds up fast.

## 2.2 The Solution

We store every value in exactly 8 bytes by exploiting IEEE 754 NaN patterns. A 64-bit double has a special "Not a Number" bit pattern. There are trillions of different NaN patterns but they all mean the same thing. We steal unused patterns to encode non-number values.

## 2.3 Encoding Layout (Validated: EXP-08, zero collisions)

```
DOUBLES (zero extraction cost):
  0x0000_0000_0000_0000 to 0x7FF7_FFFF_FFFF_FFFF  = positive doubles
  0x7FF8_0000_0000_0000                             = canonical NaN
  0x8000_0000_0000_0000 to 0xFFF7_FFFF_FFFF_FFFF  = negative doubles

TAGGED VALUES (negative-NaN space):
  0xFFF8_0000_XXXX_XXXX = 32-bit signed integer
  0xFFF9_0000_0000_XXXX = heap pointer (48-bit)
  0xFFFA_0000_0000_0000 = undefined
  0xFFFA_0000_0000_0001 = null
  0xFFFA_0000_0000_0002 = true
  0xFFFA_0000_0000_0003 = false
  0xFFFB_0000_XXXX_XXXX = symbol ID
  0xFFFC_0000_XXXX_XXXX = atom index (interned string ref)
  0xFFFD_LLLL_DDDD_DDDD = inline short string (up to 7 ASCII chars)
```

## 2.4 Core Operations (C implementation)

```c
// Type checks - each is one comparison or mask
#define R8E_IS_DOUBLE(v)   ((v) < 0xFFF8000000000000ULL)
#define R8E_IS_INT32(v)    (((v) >> 32) == 0xFFF80000U)
#define R8E_IS_POINTER(v)  (((v) >> 32) == 0xFFF90000U)
#define R8E_IS_INLINE_STR(v) (((v) >> 48) == 0xFFFDU)

// Extraction - zero cost for doubles, one mask for everything else
static inline double r8e_get_double(uint64_t v) {
    double d; memcpy(&d, &v, 8); return d;  // compiles to single mov
}
static inline int32_t r8e_get_int32(uint64_t v) {
    return (int32_t)(v & 0xFFFFFFFF);
}
static inline void* r8e_get_pointer(uint64_t v) {
    return (void*)(v & 0x0000FFFFFFFFFFFFULL);
}

// Encoding
static inline uint64_t r8e_from_double(double d) {
    uint64_t v; memcpy(&v, &d, 8); return v;
}
static inline uint64_t r8e_from_int32(int32_t i) {
    return 0xFFF8000000000000ULL | (uint32_t)i;
}
static inline uint64_t r8e_from_pointer(void *p) {
    return 0xFFF9000000000000ULL | (uint64_t)p;
}

#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL
```

## 2.5 Inline Short Strings (Bonus from EXP-05)

EXP-05 showed 50% of strings are 8 bytes or shorter and 91.5% are ASCII. We encode strings up to 7 ASCII chars directly in the NaN-box payload. No heap allocation, no GC, no string table lookup. Covers ~40-45% of all string values.

```c
// Tag 0xFFFD: bits [47:45] = length (0-7), bits [44:0] = packed chars (7 bits each)
static inline uint64_t r8e_from_inline_str(const char *s, int len) {
    uint64_t v = 0xFFFD000000000000ULL;
    v |= ((uint64_t)len << 45);
    for (int i = 0; i < len && i < 7; i++)
        v |= ((uint64_t)(uint8_t)s[i] << (38 - i * 7));
    return v;
}
```

## 2.6 32-bit Fallback

On 32-bit platforms, use a discriminated union: `struct { uint32_t tag; uint32_t payload; }` = 8 bytes. Doubles split across both fields. Compile-time decision via `#ifdef`.

---

# 3. OBJECT MODEL (4-Tier CDOL)

## 3.1 The Problem

JavaScript objects are dictionaries mapping string keys to values. Object sizes vary wildly (0 to 1000+ properties). V8 uses "hidden classes" (Maps) which are brilliant for JIT but useless for an interpreter and add ~50 bytes overhead per unique shape.

## 3.2 Experiment Result (EXP-01)

98.8% of objects have 4 or fewer properties. Detailed breakdown:
- 0 properties: 5.1% (44K objects)
- 1 property: 50.1% (437K objects)
- 2 properties: 15.4% (134K objects)
- 3 properties: 16.8% (147K objects)
- 4 properties: 11.5% (100K objects)
- 5-16 properties: 1.1% (10K objects)
- 17+ properties: less than 0.01%

## 3.3 Four-Tier Design

### Tier 0: Micro Object (0-1 properties) - 24 bytes

55% of all objects. Just a header plus one inline key-value slot.

```c
typedef struct {
    uint32_t flags;      // GC bits, tier tag (2 bits), frozen flag
    uint32_t proto_id;   // index into prototype table
    uint64_t key0;       // atom index of single property (0 = empty)
    uint64_t val0;       // NaN-boxed value
} R8EObjTier0;           // 24 bytes
```

For comparison: QuickJS uses ~96 bytes for ANY object (header + shape + property array).

### Tier 1: Compact Object (2-4 properties) - 40-72 bytes

44% of all objects. Header plus inline array of up to 4 key-value pairs.

```c
typedef struct {
    uint32_t flags;
    uint32_t proto_id;
    uint8_t  count;      // 2-4
    uint8_t  pad[7];
    struct { uint64_t key; uint64_t val; } props[4];
} R8EObjTier1;           // 8 + 8 + (16 * count) bytes
```

Property lookup: linear scan of 2-4 entries. On modern CPUs with 64-byte cache lines, this is 1-2 cache reads, faster than any hash table.

### Tier 2: Compact Array (5-16 properties) - heap-allocated

~1% of objects. Header + pointer to separate key-value array.

```c
typedef struct {
    uint32_t flags;
    uint32_t proto_id;
    uint8_t  count;
    uint8_t  capacity;   // power of 2: 8 or 16
    uint8_t  pad[6];
    struct { uint64_t key; uint64_t val; } *props;  // heap array
} R8EObjTier2;           // 16 byte header + separate allocation
```

Lookup: linear scan for count <= 8, binary search (on sorted atom indices) for 9-16.

### Tier 3: Hash Table (17+ properties) - rare

Less than 0.01% of objects. Robin Hood open-addressing hash table.

```c
typedef struct {
    uint32_t flags;
    uint32_t proto_id;
    uint16_t count;
    uint16_t capacity;   // power of 2
    uint32_t pad;
    struct { uint64_t key; uint64_t val; } *buckets;
} R8EObjTier3;
```

Robin Hood hashing: on collision, the entry that traveled farther from its ideal position gets priority. Bounds worst-case probe length to O(log n).

### Tier Transitions

Objects promote when they outgrow their tier. They never demote (avoids thrashing; 99% never promote past Tier 1). Promotion copies existing properties to the new layout.

### Array Optimization

Arrays use a separate fast path: contiguous C array of NaN-boxed values indexed by integer. No key storage needed. Named properties (arr.customProp) stored in a separate Tier0/1 object, only allocated if needed.

```c
typedef struct {
    uint32_t  flags;       // includes IS_ARRAY flag
    uint32_t  proto_id;    // PROTO_ARRAY
    uint32_t  length;
    uint32_t  capacity;
    uint64_t *elements;    // dense NaN-boxed array
    R8EObject *named;      // NULL if no named properties (common case)
} R8EArray;
```

Sparse arrays (length >> actual elements): switch to hash table of index-to-value pairs when length > 2 * element_count.

### Prototype Chains

Instead of a full pointer to prototype, store proto_id (index into realm's prototype table, typically less than 50 entries). Prototype chain walk: follow proto_id links until property found or chain ends.

---

# 4. STRING ENGINE

## 4.1 Experiment Results (EXP-05)

- 91.5% of strings are pure ASCII
- 99.3% of individual characters are ASCII
- 50.3% of strings are 8 bytes or shorter
- Only 0.002% of strings contain characters above U+FFFF (surrogates)

## 4.2 Three-Tier String Storage

**Tier A - Inline Short String (0-7 ASCII bytes, ~40-45% of strings):**
Stored entirely in the NaN-box payload. Zero heap allocation. See section 2.5.

**Tier B - Heap ASCII String (8+ bytes, pure ASCII, ~47% of strings):**
Header + raw bytes. Random access str[i] is just data[i]. O(1).

**Tier C - Heap Unicode String (non-ASCII, ~8.5% of strings):**
Header + UTF-8 bytes. Random access requires decoding. Lazy offset table built on first random access.

## 4.3 String Header

```c
typedef struct {
    uint32_t flags;        // IS_ASCII, IS_INTERNED, IS_ROPE, GC bits
    uint32_t hash;         // precomputed (for property lookups)
    uint32_t byte_length;  // UTF-8 byte count
    uint32_t char_length;  // UTF-16 code unit count (= byte_length if ASCII)
    // char data[];        // UTF-8 bytes follow immediately (flexible array)
} R8EString;               // 16 byte header
```

"hello" = 16 + 5 = 21 bytes (+ alignment). QuickJS uses 32+ byte header.

## 4.4 Lazy Offset Table

For the rare non-ASCII strings where someone does str[i]:

```c
// Built on first random access, then cached
typedef struct {
    uint32_t *offsets;  // offsets[i] = byte position of i-th UTF-16 code unit
    uint32_t  length;
} R8EOffsetTable;
```

Most string operations (iteration, indexOf, slice, concat) work sequentially and never need this table. It is only built for bracket indexing on non-ASCII strings.

## 4.5 String Interning (Atom Table)

Property names are interned: stored once, referenced by 32-bit atom index. Comparing two interned strings = comparing two integers.

```c
typedef struct {
    uint64_t bloom[4];      // 256-bit Bloom filter for fast "not present" check
    uint32_t count;
    uint32_t capacity;
    R8EAtomEntry *entries;  // hash table: {hash, string_ptr, next}
} R8EAtomTable;
```

Pre-populated with ~256 common names: "length", "prototype", "constructor", "toString", etc. Bloom filter avoids hash table lookup for strings that definitely are not interned (common during parsing of new identifiers).

---

# 5. SINGLE-PASS COMPILER

## 5.1 The Problem (EXP-07)

Most JS engines build an AST (Abstract Syntax Tree) then compile it to bytecode. EXP-07 showed QuickJS uses 23-35x source file size in peak memory for this. A 1MB bundle = 23-35MB temporary RAM.

## 5.2 Our Approach: Direct Bytecode Emission

Parse source in a single forward pass, emitting bytecode directly. No AST ever built. Parser reads tokens, decides what construct it sees, writes bytecode to output buffer.

Target: less than 1.5x source size in peak memory (vs 23-35x for AST approach).

## 5.3 The Ambiguity Problem

JavaScript has grammatical ambiguities requiring lookahead:

```javascript
(a, b)         // parenthesized comma expression?
(a, b) => a+b  // arrow function parameters!

{ x: 1 }       // block with label "x"?  or object literal?
```

## 5.4 Speculation and Patch

Handle ambiguities with speculation-and-patch:

1. See "(" - assume parenthesized expression, emit bytecode for that
2. Later see "=>" - now we know it was arrow params
3. PATCH: rewrite already-emitted bytecode to reinterpret values as param bindings

Patch points stored on a small stack (8 bytes each, rarely exceeds 4-5 deep).

```
Parsing "(a, b) => a + b":
  See "("  -> could be grouping or arrow
  See "a"  -> emit LOAD_VAR "a" (works for both)
  See ","  -> record patch point
  See "b"  -> emit LOAD_VAR "b"
  See ")"  -> record end
  See "=>" -> PATCH: rewrite LOAD_VARs as PARAM_BINDs, emit FUNC_START
  Continue compiling body normally
```

## 5.5 Scope Tracking

Without an AST, track scopes with a compile-time scope stack:

```c
typedef struct R8EScope {
    struct R8EScope *parent;
    uint16_t local_count;
    uint16_t local_base;      // starting register index
    uint8_t  flags;           // HAS_EVAL, IS_STRICT, etc.
    R8EVarInfo vars[MAX_LOCALS];
} R8EScope;

typedef struct R8EVarInfo {
    uint32_t atom;            // variable name (interned)
    uint8_t  register_idx;
    uint8_t  classification;  // OWNED, BORROWED, or TEMP
    uint8_t  flags;           // IS_CONST, IS_CAPTURED, IS_MUTATED_AFTER_CAPTURE
} R8EVarInfo;
```

## 5.6 RC Classification at Compile Time (EXP-03: 53-61% elidable)

During compilation, every variable classified as:
- **OWNED**: stored into property, captured by closure, or returned. Must do RC.
- **BORROWED**: loaded from local/property, used in same scope, never stored elsewhere. Skip RC.
- **TEMP**: intermediate result consumed immediately. Skip RC entirely.

53-61% elidable from classification alone. Chain superinstructions push this to ~70-75% (section 6).

## 5.7 Eval Handling: Lazy Scope Materialization

When parser encounters eval(), set HAS_EVAL flag but compile OPTIMISTICALLY (keep all classifications). Emit EVAL_TRAP opcode. At runtime, if trap fires:
1. Parse eval string to bytecode
2. Examine what it actually accesses (intersect with scope descriptor)
3. Only materialize (spill to heap) variables the eval code touches
4. Patch bytecode at those variable access sites
5. Variables NOT accessed by eval remain optimized

This is novel: V8 and QuickJS pessimize the entire scope chain at compile time when eval is present. We defer to runtime and only pay for what eval actually uses.

---

# 6. BYTECODE FORMAT AND SUPERINSTRUCTIONS

## 6.1 Variable-Width Encoding (EXP-06: 44% savings)

EXP-06 showed entropy of 4.23 bits/opcode, with top-16 opcodes covering 90.2% of dispatches.

Encoding scheme:
- **Top 16 opcodes: 4-bit prefix** (half a byte) - covers 90% of dispatches
- **Next 48 opcodes: 8-bit code** - covers 9.8%
- **Remaining: 16-bit code** - covers 0.2%

Total bytecode size reduction: ~44% vs fixed 1-byte opcodes.

The decoder hot path: read 4 bits, lookup in 16-entry table, done. For 90% of instructions, decoding is a single nibble read.

## 6.2 Chain Superinstructions (EXP-02: 78.7% coverage from top-30)

EXP-02 revealed that instruction bigrams form CHAINS, not random pairs. The top patterns:

| Chain | Opcodes Fused | Dispatches | What It Does |
|---|---|---|---|
| CALL_METHOD_FAST | get_var + get_field2 + get_loc_check + call_method | 17.8% | Method call (obj.method()) |
| FOR_INC_LOOP | get_loc + post_inc + put_loc + drop + goto8 | 18.0% | for-loop i++ |
| LOOP_CMP_BRANCH | get_loc + push_i32 + lt + if_false8 | ~7% | for-loop condition (i < n) |
| LOAD_FIELD_CHECK | get_field2 + get_loc_check | ~6% | Property access |
| BRANCH_LOAD | if_false8 + get_loc_check | ~3% | Branch then load |

Five to seven chain superinstructions cover 50%+ of all dispatches. Each chain does in one dispatch what previously required 3-5.

### Why Chains Beat Pairs

A pair fuses 2 instructions and saves 1 dispatch. A chain fuses 4 instructions and saves 3. But the real win is deeper: inside a chain, the intermediate values flow through registers without touching the value stack. This means:
- Zero RC operations within the chain (the values never "escape")
- Zero stack pushes/pops between fused ops
- The chain handler can use CPU registers for intermediates

This is how chain superinstructions boost RC elision from 53-61% to ~70-75%.

## 6.3 Opcode Table (Initial Set)

```
Category: Load/Store
  LOAD_LOCAL      reg         - load local variable from register
  STORE_LOCAL     reg         - store to local variable
  LOAD_GLOBAL     atom        - load global by name
  STORE_GLOBAL    atom        - store to global
  LOAD_CONST      idx         - load from constant pool
  PUSH_INT        imm8/16/32  - push integer literal
  PUSH_DOUBLE     idx         - push double from constant pool

Category: Property Access
  GET_PROP        atom        - obj.name
  SET_PROP        atom        - obj.name = val
  GET_ELEM                    - obj[key]
  SET_ELEM                    - obj[key] = val

Category: Arithmetic
  ADD, SUB, MUL, DIV, MOD    - binary arithmetic
  NEG, POS                   - unary arithmetic
  BITAND, BITOR, BITXOR      - bitwise
  SHL, SHR, USHR             - shifts

Category: Comparison/Logic
  EQ, SEQ, NE, SNE           - equality (== === != !==)
  LT, LE, GT, GE             - relational
  NOT, TYPEOF, INSTANCEOF, IN

Category: Control Flow
  JUMP            offset      - unconditional jump
  JUMP_IF_FALSE   offset      - conditional branch
  JUMP_IF_TRUE    offset
  CALL            argc        - function call
  CALL_METHOD     argc        - method call
  RETURN                      - return from function
  THROW                       - throw exception

Category: Object/Array
  NEW_OBJECT                  - create {}
  NEW_ARRAY       count       - create [] with count elements
  NEW_FUNCTION    idx         - create closure from function index

Category: Superinstructions (5-7 chains)
  CALL_METHOD_FAST  atom,argc - fused method lookup + call
  FOR_INC_LOOP      reg       - fused i++ loop increment
  LOOP_CMP_BRANCH   reg,imm,off - fused loop condition test
  LOAD_FIELD_CHECK  atom      - fused property load + check
  BRANCH_LOAD       off,reg   - fused branch + load

Category: Special
  EVAL_TRAP                   - deferred eval handling
  CHECK_LIMITS                - resource limit check (at loop back-edges)
  NOP                         - padding
```

---

# 7. INTERPRETER AND DISPATCH

## 7.1 Computed Goto (GCC/Clang)

The interpreter main loop uses computed goto (GCC extension) for direct threading. Instead of a switch statement (indirect branch through a table), each opcode handler ends with a direct jump to the next handler.

```c
// Dispatch table: array of label addresses
static const void *dispatch_table[] = {
    &&op_load_local, &&op_store_local, &&op_load_global, ...
};

#define DISPATCH() goto *dispatch_table[*pc++]

// Each handler:
op_add: {
    uint64_t b = *--sp;
    uint64_t a = *--sp;
    if (R8E_IS_INT32(a) && R8E_IS_INT32(b)) {
        int64_t r = (int64_t)r8e_get_int32(a) + r8e_get_int32(b);
        if (r >= INT32_MIN && r <= INT32_MAX)
            *sp++ = r8e_from_int32((int32_t)r);
        else
            *sp++ = r8e_from_double((double)r);
    } else {
        *sp++ = r8e_add_slow(ctx, a, b);  // handles string concat, etc.
    }
    DISPATCH();
}
```

## 7.2 Chain Superinstruction Handler Example

```c
op_for_inc_loop: {
    // Fused: get_loc(reg) + post_inc + put_loc(reg) + drop + goto8(offset)
    uint8_t reg = *pc++;
    int8_t  offset = (int8_t)*pc++;
    
    uint64_t val = locals[reg];
    
    // Fast path: int32 that does not overflow
    if (R8E_IS_INT32(val)) {
        int32_t i = r8e_get_int32(val);
        if (i < INT32_MAX) {
            locals[reg] = r8e_from_int32(i + 1);
            // ZERO RC operations - value never leaves registers
            pc += offset;
            DISPATCH();
        }
    }
    // Slow path: handle doubles, overflow, non-numeric
    r8e_for_inc_slow(ctx, reg, offset);
    DISPATCH();
}
```

Key points: the fast path (integer loop counter that does not overflow) is 3-4 machine instructions with zero function calls, zero RC operations, zero stack manipulation. This covers the vast majority of for-loop increments.

## 7.3 Portable Fallback

On compilers without computed goto (MSVC), fall back to a switch-based dispatch:

```c
for (;;) {
    switch (*pc++) {
        case OP_LOAD_LOCAL: ...  break;
        case OP_ADD:        ...  break;
        ...
    }
}
```

~15-20% slower than computed goto due to indirect branch prediction. Acceptable for portability.

---

# 8. GARBAGE COLLECTION

## 8.1 Strategy: RC with Compile-Time Elision + Epoch Cycle Detection

We use reference counting (not tracing GC) because:
- Deterministic: no pauses, predictable latency
- Immediate: objects freed as soon as unreachable
- Lower memory: no need to keep dead objects until next GC cycle
- Simpler: ~2000 LOC vs ~5000+ for a generational tracing GC

The downside of RC: circular references leak. We handle this with periodic cycle detection.

## 8.2 RC Elision (EXP-03: 53-61% base, ~70-75% with superinstructions)

The compiler classifies every value operation:
- BORROWED: skip RC (value owned by someone else in same scope)
- TEMP: skip RC (consumed immediately)
- OWNED: must do RC (value escapes its scope)

Chain superinstructions eliminate additional RC ops by keeping values in CPU registers within the fused handler. The FOR_INC_LOOP chain does a read-modify-write on a local variable with zero RC operations.

### Deferred Decrement Batching

When storing a new value to a local variable, instead of immediately freeing the old value:

```c
// Instead of:
//   JS_FreeValue(ctx, locals[reg]);  // immediate decrement + maybe free
//   locals[reg] = new_val;
//   JS_DupValue(ctx, new_val);       // increment

// We do:
//   defer_free(ctx, locals[reg]);    // add to deferred list
//   locals[reg] = new_val;
//   JS_DupValue(ctx, new_val);

// At function exit, batch-free the entire deferred list:
//   for (int i = 0; i < defer_count; i++)
//       JS_FreeValue(ctx, defer_list[i]);
```

This converts N individual decrement-and-maybe-free operations into one linear scan at function exit. In tight loops, many deferred values are the same temporary, and we can deduplicate.

## 8.3 Cycle Detection (EXP-09: fixed 4096 epoch as default)

Reference counting cannot collect circular references (A points to B, B points to A, both unreachable but refcount is 1). We detect these with periodic scans.

### Suspect List

When an object's refcount decreases but stays above zero, it might be in a cycle. Add it to a suspect list.

```c
void r8e_dec_ref(R8EObject *obj) {
    if (--obj->refcount == 0) {
        r8e_free_object(obj);  // immediate collection
    } else {
        // Refcount > 0 but just decreased - might be in a cycle
        r8e_suspect_list_add(obj);
    }
}
```

When the suspect list exceeds 4096 entries (the default epoch threshold from EXP-09), trigger a scan:

```c
void r8e_cycle_scan(R8EContext *ctx) {
    // Mark-and-sweep on suspect list only (not entire heap)
    // 1. Trial decrement: pretend to remove all suspect-to-suspect references
    // 2. If any suspect reaches zero: it is garbage (part of an unreachable cycle)
    // 3. Free the garbage, restore trial decrements for survivors
    
    // Adaptive threshold:
    if (cycles_found > 0) {
        ctx->epoch_threshold = MAX(256, ctx->epoch_threshold / 2);
    } else {
        ctx->epoch_threshold = MIN(65536, ctx->epoch_threshold * 2);
    }
}
```

## 8.4 Object Header GC Bits

Every heap object has a minimal header:

```c
// 4 bytes of flags contain everything GC needs:
// Bits [1:0]   = tier (0-3) for objects, or type tag for non-objects
// Bit  [2]     = GC mark (used during cycle scan)
// Bit  [3]     = on suspect list
// Bit  [4]     = frozen (Object.freeze)
// Bits [7:5]   = reserved
// Bits [15:8]  = refcount overflow (if refcount > 255, use external counter)
// Bits [31:16] = inline refcount (0-65535 covers 99.99% of objects)
```

---

# 9. CLOSURES AND SCOPES (PVCC)

## 9.1 The Problem

A closure is a function that "remembers" variables from its surrounding scope:

```javascript
function makeCounter() {
    let count = 0;
    return function() { return count++; };
}
```

The inner function captures `count`. After makeCounter returns, `count` normally dies (it is a local). But the closure keeps it alive.

Traditional approach: create a heap-allocated "upvalue box" for each captured variable. Adds indirection, allocation, GC pressure.

## 9.2 PVCC: Per-Variable Capture Classification (EXP-04: 83.1% immutable)

EXP-04 showed 83.1% of captured variables are never mutated after capture, and 70.4% of closures have ALL captures immutable.

For immutable captures: COPY the value directly into the closure object. No upvalue box, no indirection, no GC tracking on the upvalue. Just memcpy the NaN-boxed value.

For mutable captures: use traditional shared upvalue box (heap-allocated cell that both the closure and the enclosing scope reference).

The compiler decides at compile time by tracking IS_MUTATED_AFTER_CAPTURE in R8EVarInfo.

## 9.3 Tiered Capture Storage (Adjusted from EXP-04: avg 6.2 captures)

EXP-04 showed average captures/closure = 6.2 (higher than expected 2-3, due to bundled code). Distribution peaked at 1-3 but had a fat tail.

| Captures | Storage | Coverage |
|---|---|---|
| 1-2 | Inline in closure object (two pointer-sized slots) | ~52% |
| 3-8 | Separate small heap array | ~28% |
| 9+ | Shared environment frame (traditional) | ~20% |

```c
typedef struct R8EClosure {
    R8EFunction *func;     // bytecode + metadata
    uint8_t capture_count;
    uint8_t capture_mode;  // INLINE(0), ARRAY(1), FRAME(2)
    union {
        uint64_t inline_captures[2];    // mode 0: two NaN-boxed values
        uint64_t *capture_array;        // mode 1: heap array
        R8EEnvFrame *env_frame;         // mode 2: shared frame
    };
} R8EClosure;
```

For the 70% of closures where ALL captures are immutable and count is 1-2: the closure is a single allocation of ~32 bytes with zero external references. Extremely GC-friendly.

---

# 10. REGULAR EXPRESSION ENGINE

## 10.1 Tiered by Pattern Complexity (EXP-10: 1.34x for simple, more for complex)

EXP-10 showed that for simple patterns (no nested quantifiers), backtracking is nearly as fast as NFA. The NFA advantage appears with complex patterns where backtracking goes exponential.

**Decision rule (at regex compile time):**
- Simple pattern (no nested quantifiers, no alternations, fewer than 3 groups): use BACKTRACKING
- Complex pattern (nested * or + inside groups, or more than 3 alternations): use BITSET NFA
- Threshold: estimate at compile time from regex AST structure

## 10.2 Backtracking Engine (Simple Patterns)

Traditional recursive backtracking with a fuel counter to prevent ReDoS:

```c
#define R8E_REGEX_FUEL_MAX 1000000  // max steps before giving up

int r8e_regex_backtrack(R8ERegex *re, const char *input, int pos) {
    int fuel = R8E_REGEX_FUEL_MAX;
    return bt_match(re, input, pos, 0, &fuel);
}

// If fuel hits zero: return "no match" (prevents exponential blowup)
// This is a safety net - simple patterns never exhaust fuel
```

## 10.3 Bitset NFA Engine (Complex Patterns, 95% have fewer than 64 states)

For patterns with fewer than 64 NFA states (95% of JS regexes), the entire state set fits in a single uint64_t:

```c
typedef struct {
    uint64_t char_masks[256];  // bit i set if state i transitions on this char
    uint64_t accept_mask;
    uint8_t  num_states;
} NFA64;

static inline uint64_t nfa64_step(NFA64 *nfa, uint64_t state, uint8_t ch) {
    return ((state << 1) | 1) & nfa->char_masks[ch];
}

// ~4-8 CPU cycles per character. Zero heap allocation during matching.
```

For 65-256 states: use AVX2 256-bit SIMD registers (when available) with the same algorithm. For 257+ states (extremely rare in JS): fall back to sparse bitset array.

---

# 11. SECURITY ARCHITECTURE (5 Layers)

## 11.1 Overview

Five layers of defense-in-depth, adding ~16KB to binary (~6,300 LOC). Each layer defends against different threat classes. If one layer fails, the others still provide protection.

## 11.2 Threat Model

**Must defend against:**
- T1: Untrusted JS execution (malicious plugins, user scripts)
- T2: Supply chain attacks (compromised npm packages)
- T3: Memory bugs in our C engine
- T4: Resource exhaustion (infinite loops, memory bombs)

**Should defend against:**
- T5: Side channels (timing attacks)
- T6: Prototype pollution

## 11.3 Layer 1: OS-Level Sandboxing (~500 LOC, ~1.5KB)

Seccomp-BPF syscall filtering with OpenBSD pledge-style API. Kernel enforces restrictions even if attacker achieves code execution.

```c
// JS-facing API:
r8e.pledge("stdio rpath");  // allow only stdio + read-only filesystem

// After this call, the kernel BLOCKS all other syscalls:
// - No network (connect, socket, accept)
// - No file creation (open with O_CREAT)  
// - No process spawning (execve, fork, clone)
// Even a buffer overflow exploit cannot make network connections.
```

EXP-11 validated: pure computation needs only ~15 syscalls. pledge("stdio") is sufficient for 90% of sandboxed workloads.

Two-phase narrowing: load script with rpath (read files), then drop to stdio-only before execution. One-way ratchet: permissions can only be removed, never added back.

Landlock (Linux 5.13+) for path-based filesystem isolation: restrict which directories each realm can access.

## 11.4 Layer 2: Memory Safety Hardening (~1,200 LOC, ~3KB)

### Arena Allocator with Guard Pages

Each realm gets its own memory arena bounded by unmapped pages. Buffer overflow = immediate SIGSEGV.

```c
// Arena creation (EXP-12: 128KB default, EXP-13: <2us creation time):
void *arena = mmap(guard_page + 128KB_arena + guard_page);
mprotect(guard_page_low, PROT_NONE);   // unmapped: crash on underflow
mprotect(guard_page_high, PROT_NONE);  // unmapped: crash on overflow
```

### NaN-Box Validation at Trust Boundaries

Every time a value crosses from native C code to JS or vice versa, validate the NaN-box tag bits. Catches memory corruption before it propagates.

### Stack Canaries

Magic value 0xDEADBEEF42424242 placed at bytecode frame boundaries, checked on function return. Detects stack buffer overwrites.

### Bounds-Checked TypedArrays

TypedArrays use 4GB virtual memory reservation (mostly uncommitted). Out-of-bounds access hits unmapped pages = SIGSEGV. Zero runtime bounds-check cost via MMU.

Embedded fallback (no virtual memory): branchless bounds clamping:
`idx = idx - ((idx - length) & ~((int32_t)(idx - length) >> 31))`

## 11.5 Layer 3: Bytecode Verification (~800 LOC, ~2KB)

Wasm-style pre-execution verification. No existing lightweight JS engine does this. Single O(n) forward pass guarantees:
- Stack safety: no underflow or overflow
- Type safety: operations match expected types
- Control flow integrity: all jumps land on valid instruction boundaries
- Constant pool bounds: all constant references are in-range
- Local variable bounds: all register accesses are valid

Runs once at module load. Less than 1ms for a 50KB module. Catches compiler bugs before execution. EXP-15 generated 1,200 fuzz test inputs for validation.

## 11.6 Layer 4: Context Isolation (Multi-Realm) (~2,000 LOC, ~5KB)

Lightweight V8-Isolate equivalents: ~64KB per realm vs ~2MB for V8 Isolate (EXP-13: creation in <2us, 5000x faster than V8).

Each realm has:
- Dedicated arena (isolated memory)
- Independent prototype chain (own Object/Array/Function prototypes)
- Own global object
- Shared immutable data only (atom table, bytecode cache)

Cross-realm communication: structured clone only (EXP-14: <10us for <1KB messages). No direct object references between realms.

Novel tagged arenas: realm ID embedded in NaN-boxed pointer upper bits (4 bits = 16 max realms). Cross-realm reference detected in O(1) with zero memory overhead.

Resource limits per realm:
```c
typedef struct {
    size_t   max_memory;        // bytes
    uint64_t max_cpu_time_us;   // microseconds
    uint32_t max_stack_depth;
    uint32_t max_string_length;
    uint32_t max_array_length;
    uint32_t max_regexp_steps;
} R8ERealmLimits;
```

CPU enforcement: CHECK_LIMITS opcode inserted at loop back-edges and function calls.

## 11.7 Layer 5: Capability-Based API Surface (~1,500 LOC, ~4KB)

Object-capability model (ocap): no ambient authority. Code cannot use capabilities it does not have a reference to.

NO global fs/net/process APIs. Capabilities must be explicitly passed:

```javascript
// Host creates restricted capability:
const fsRead = r8e.createCapability("fs", {
    root: "/app/data",
    permissions: "read",
    quotaMB: 100
});

// Pass to untrusted code:
runPlugin(pluginCode, { fs: fsRead });
// Plugin can read /app/data but nothing else
// Even if plugin has a memory corruption exploit,
// Layer 1 (seccomp) still blocks raw syscalls
```

Capability attenuation: restrict before passing to less-trusted code:
```javascript
const fsNarrow = fsRead.attenuate({ root: "/app/data/public" });
```

## 11.8 Additional Protections

**Frozen intrinsics:** All built-in prototypes (Object.prototype, Array.prototype, etc.) frozen at realm creation. Prevents prototype pollution attacks.

**No SharedArrayBuffer between realms:** Prevents Spectre-class side channels.

**Timer resolution clamping:** 1ms minimum (vs 5us native). Makes timing attacks impractical.

**No JIT:** Eliminates ~45% of Chrome's in-the-wild exploits (which exploit JIT code generation).

---

# 12. UI RENDERING STACK (NanoUI)

## 12.1 Why Not WebKit

WebKit is 15M+ LOC. Our entire binary budget is 166KB. We build a minimal UI stack that covers what 95% of UI code actually uses.

## 12.2 Layer 1: NanoDOM (~2,000 LOC)

Minimal document tree. NOT the full W3C DOM. Just:
- createElement, createTextNode
- appendChild, removeChild, insertBefore
- setAttribute, getAttribute
- addEventListener, removeEventListener
- getElementById, querySelectorAll (CSS selectors)
- className, style (inline styles)

DOM node attributes reuse our CDOL object model. Zero extra data structures.

```c
typedef struct NanoDOMNode {
    uint8_t           type;        // ELEMENT, TEXT, DOCUMENT, FRAGMENT
    uint32_t          flags;       // dirty, visible, focusable
    uint32_t          tag_atom;    // interned tag name
    R8EObject        *attrs;       // Tier0/1 CDOL object for attributes
    R8EArray         *class_list;  // sorted atom array for fast matching
    struct NanoDOMNode *parent, *first_child, *last_child, *next_sibling;
    NanoLayoutBox     layout;      // computed position/size
    NanoComputedStyle computed;     // resolved CSS values
} NanoDOMNode;
```

## 12.3 Layer 2: NanoStyle (~2,500 LOC)

CSS subset engine. ~60 properties (not 500+). Supports:
- display (flex, block, inline, none)
- position (relative, absolute, fixed)
- Box model (width, height, padding, margin, border)
- Flexbox (flex-direction, justify-content, align-items, flex-grow, flex-shrink, flex-basis, gap, flex-wrap)
- Colors (color, background-color, border-color, opacity)
- Typography (font-size, font-weight, font-family, line-height, text-align)
- Visual (border-radius, overflow, visibility, cursor, box-shadow basic)
- Selectors: tag, .class, #id, combinators (space, >), pseudo (:hover, :focus, :active, :first-child, :last-child)

NOT supported (initially): float, table layout, grid, animations, transforms, media queries, custom properties.

Selector matching uses atom interning: class names are integers, matching is integer comparison. Zero string operations during style resolution.

## 12.4 Layer 3: NanoLayout (~3,500 LOC)

Flexbox + Block layout. No float, no table, no grid (initially).

Why flexbox is enough: React Native ships with ONLY flexbox (via Yoga, ~8,300 LOC) and powers Instagram, Facebook, Discord, Shopify. We do it in ~3,500 LOC by integrating directly with our DOM.

Two-pass layout algorithm:
1. Measure (bottom-up): determine intrinsic sizes
2. Layout (top-down): assign final positions

```c
typedef struct NanoLayoutBox {
    float x, y;           // position relative to parent content box
    float width, height;  // content area
    float padding[4];     // TRBL
    float border[4];
    float margin[4];
} NanoLayoutBox;
```

## 12.5 Layer 4: NanoPaint (~2,000 LOC)

Generates flat display list of draw commands, feeds to:
- NanoVG (~5,000 LOC external): GPU-accelerated 2D via OpenGL. What Ladybird started with.
- Software rasterizer (~1,500 LOC): direct pixel buffer for headless/embedded.

Display list caching: hash the entire display list. If hash matches previous frame, skip GPU render. Mostly-static UI = zero GPU work per frame.

## 12.6 Comparison

| | Binary | CSS | GPU | JS Engine |
|---|---|---|---|---|
| Electron | ~200MB | Full | Yes | V8 |
| WebKit | ~50MB | Full | Yes | JSC |
| Sciter | ~5MB | Most CSS3 | Yes | Custom |
| React Native | ~8MB | Flexbox | Via native | Hermes |
| **r8e+NanoUI** | **~166KB** | **Flex+Block** | **Yes** | **r8e** |

---

# 13. FILE STRUCTURE AND BUILD SYSTEM

## 13.1 Source Tree

```
r8e/
  Makefile
  include/
    r8e_types.h          -- NaN-boxing, value macros
    r8e_opcodes.h        -- opcode enum + superinstructions
    r8e_atoms.h          -- pre-interned atom table (256 common names)
    r8e_api.h            -- public API
  src/
    r8e_value.c          -- value creation, type checks, conversions
    r8e_alloc.c          -- arena allocator, slab allocator
    r8e_number.c         -- number ops (int32 fast path + double slow)
    r8e_string.c         -- string creation, interning, UTF-8 ops
    r8e_atom.c           -- atom table, Bloom filter
    r8e_token.c          -- lexer/tokenizer (streaming, UTF-8)
    r8e_parse.c          -- single-pass parser + bytecode emitter
    r8e_bc.c             -- bytecode buffer, patch points
    r8e_scope.c          -- scope stack, variable resolution, RC class.
    r8e_interp.c         -- interpreter main loop, dispatch
    r8e_object.c         -- 4-tier CDOL, property access
    r8e_array.c          -- array fast path, sparse fallback
    r8e_function.c       -- function objects, call mechanics
    r8e_closure.c        -- PVCC, tiered capture storage
    r8e_gc.c             -- RC ops, deferred decrement, cycle detection
    r8e_regexp.c         -- tiered regex (backtrack + NFA64)
    r8e_error.c          -- error objects, stack traces
    r8e_module.c         -- ESM import/export
    r8e_builtin.c        -- Object, Array, String, etc. built-ins
    r8e_json.c           -- JSON.parse / JSON.stringify
    r8e_promise.c        -- Promise, async/await, microtask queue
    r8e_iterator.c       -- iterators, generators
    r8e_proxy.c          -- Proxy, Reflect
    r8e_weakref.c        -- WeakRef, FinalizationRegistry
  src/security/
    r8e_sandbox.c        -- seccomp-BPF, pledge, Landlock
    r8e_arena.c          -- guard-page arenas
    r8e_verify.c         -- bytecode verifier (O(n) single pass)
    r8e_realm.c          -- multi-realm isolation
    r8e_capability.c     -- ocap API surface
  src/ui/
    ndom.c               -- NanoDOM tree
    nstyle.c             -- NanoStyle CSS subset
    nlayout.c            -- NanoLayout flexbox
    npaint.c             -- NanoPaint display list
    nevent.c             -- event dispatch, hit testing
  tests/
    test_runner.c        -- minimal test harness
    unit/                -- per-module unit tests
    test262/             -- test262 harness adapter
  bench/
    bench_runner.c       -- benchmark harness
```

## 13.2 Build System

Single Makefile with per-module compilation:

```makefile
CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wpedantic -fno-strict-aliasing
CFLAGS += -DR8E_COMPUTED_GOTO  # enable computed goto dispatch

# Sanitizer builds
debug: CFLAGS += -g -fsanitize=address,undefined -DR8E_DEBUG
release: CFLAGS += -Os -DNDEBUG -flto
size: CFLAGS += -Oz -DNDEBUG -flto -ffunction-sections -fdata-sections
size: LDFLAGS += -Wl,--gc-sections

# Per-platform
ifeq ($(shell uname),Linux)
  CFLAGS += -DR8E_HAS_SECCOMP -DR8E_HAS_LANDLOCK
endif
```

No autotools, no cmake, no configure scripts. One Makefile, one make command.

---

# 14. ENGINEERING TIMELINE (Parallel Agents)

## 14.1 Overview

~32-35 working days with 6 parallel Claude Code agents. ~7 calendar weeks.

## 14.2 Phase 0: Foundation (Days 1-2)

One agent builds shared headers and contracts that everything depends on:
- r8e_types.h (NaN-boxing encoding)
- r8e_opcodes.h (opcode enum)
- r8e_atoms.h (common names)
- Makefile with all targets
- Interface contracts (function signatures) for every module
- Coding standards document

## 14.3 Phase 1: Core Modules (Days 2-6) - 3 Agents Parallel

Agent 1: Value System + Allocator (4 days)
- r8e_value.c, r8e_alloc.c, r8e_number.c
- ~200 unit tests

Agent 2: Lexer + Parser + Compiler (5 days - CRITICAL PATH)
- r8e_token.c, r8e_parse.c, r8e_bc.c, r8e_scope.c
- Start with subset: expressions, var/let/const, if/while/for, functions
- ~400 unit tests

Agent 3: Strings + Atoms (4 days)
- r8e_string.c, r8e_atom.c
- Inline short strings, interning, UTF-8 ops
- ~200 unit tests

## 14.4 Phase 2: Runtime (Days 6-12) - 5 Agents Parallel

Agent 1: Object Model (4 days) - r8e_object.c, r8e_array.c
Agent 2: Interpreter (4 days) - r8e_interp.c (computed goto, superinstructions)
Agent 3: Built-in Objects (5 days) - r8e_builtin.c (Object, Array, String, Number, Boolean, Math, Date)
Agent 4: Closures + GC (4 days) - r8e_closure.c, r8e_gc.c
Agent 5: Functions + Error handling (3 days) - r8e_function.c, r8e_error.c

INTEGRATION CHECKPOINT 1 (Day 10): merge all modules, run combined tests. Budget 1 day for integration bugs.

## 14.5 Phase 3: Full Language (Days 12-20) - 5 Agents

Agent 1: Complete ES2023 syntax in parser (ongoing from Phase 1)
Agent 2: RegExp engine (tiered) - 4 days
Agent 3: Promise + async/await + generators - 4 days
Agent 4: Module system (ESM) + JSON - 3 days
Agent 5: Proxy, Reflect, WeakRef, Symbol, iterators - 4 days

INTEGRATION CHECKPOINT 2 (Day 18): full language integration.

## 14.6 Phase 3.5: UI Stack (Days 12-22, parallel) - 1 Agent

Agent 6 works independently:
- Days 12-15: NanoDOM + NanoStyle
- Days 15-19: NanoLayout (flexbox)
- Days 19-22: NanoPaint + NanoVG integration + events

## 14.7 Phase 4: Security (Days 18-24) - 2 Agents

Agent 1: seccomp/pledge + arena allocator + guard pages
Agent 2: bytecode verifier + realm isolation + capability API

## 14.8 Phase 5: Compliance + Hardening (Days 24-35)

All 6 agents: test262 grinding, fuzz testing, benchmark optimization, binary size optimization, documentation.

INTEGRATION CHECKPOINT 3 (Day 26): full system with UI.

Target: 85-90% test262 pass rate by Day 35.

## 14.9 Human Role

~1.5 hours/day: review agent output, architectural decisions at ambiguity points, run 3 integration checkpoints (half day each).

---

# 15. TEST STRATEGY

## 15.1 Unit Tests (~2,000 tests)

Each module has its own test file. Test at the boundary of each component:
- Value encoding: all edge cases (NaN, -0, infinity, max/min int, etc.)
- Parser: known JS snippets mapped to expected bytecode
- Objects: tier transitions, property CRUD, prototype chains
- Strings: ASCII/Unicode, interning, inline short strings
- GC: cycle detection, RC correctness, deferred decrement
- Security: verifier rejects malformed bytecode, realm isolation

## 15.2 test262 Compliance

Adapter that runs the test262 suite through r8e. Target: 85-90% pass rate for ES2023 features. Known exclusions:
- Annex B (legacy web compatibility features)
- Tail call optimization (not practical in an interpreter)
- Some Intl features (binary size concern)

## 15.3 Fuzz Testing

EXP-15 generated 1,200 initial fuzz inputs. Use AFL++ or libFuzzer for continuous fuzzing:
- Bytecode verifier: all malformed inputs must be rejected, none may crash
- Parser: all malformed JS must produce errors, never segfaults
- RegExp: pathological patterns must hit fuel limit, not hang
- String engine: malformed UTF-8 must be handled gracefully

Run under ASAN (AddressSanitizer) and UBSAN (UndefinedBehaviorSanitizer) for all fuzz campaigns.

## 15.4 Benchmarks

Compare against QuickJS on:
- SunSpider (classic micro-benchmarks)
- Octane (if available)
- Custom memory benchmarks (peak RSS during parse, runtime RSS)
- Binary size comparison
- Startup time (time to first bytecode execution)

Target: within 80% of QuickJS performance, at 50% of memory usage.

## 15.5 Security Testing

- Seccomp: verify blocked syscalls actually fail
- Arena: verify guard page crashes on overflow
- Realm: verify cross-realm object access is impossible
- Verifier: 100% rejection rate on fuzz corpus (EXP-15)
- Capability: verify capability attenuation works

---

# APPENDIX A: EXPERIMENT RESULTS SUMMARY

| Exp | What | Result | Impact |
|---|---|---|---|
| EXP-01 | Object property counts | 98.8% have 4 or fewer | Added Tier 0 (24B for 0-1 props) |
| EXP-02 | Superinstruction freq | 78.7% from top-30 bigrams | Use 5-7 chain fusions instead of 30 pairs |
| EXP-03 | RC elision rate | 53-61% elidable | Added deferred decrement + chain elimination |
| EXP-04 | Closure captures | 83.1% immutable, avg 6.2 | Tiered capture: inline/array/frame |
| EXP-05 | String ASCII freq | 91.5% ASCII, 50% short | Added inline short strings in NaN-box |
| EXP-06 | Bytecode compaction | 4.23 bits entropy, 44% savings | 4-bit prefix for top-16 opcodes |
| EXP-07 | Parse memory | 23-35x ratio (QuickJS) | Confirms single-pass value |
| EXP-08 | NaN-boxing safety | Zero collisions | Encoding validated |
| EXP-09 | Cycle detection | Fixed 4096 good default | Suspect-list trigger instead of alloc count |
| EXP-10 | NFA regex speed | 1.34x for simple patterns | Tier by complexity, not state count |
| EXP-11 | Syscall profiling | ~15 syscalls for compute | pledge("stdio") covers 90% |
| EXP-12 | Arena sizing | 64-256KB optimal | Default 128KB, 64KB for plugins |
| EXP-13 | Realm creation | less than 2us | 5000x faster than V8 Isolate |
| EXP-14 | Structured clone | 1-9us for less than 1KB | No need for COW optimization |
| EXP-15 | Fuzz corpus | 1200 inputs generated | Ready for verifier testing |

---

# APPENDIX B: DESIGN DECISIONS LOG

| Decision | Options Considered | Chosen | Why |
|---|---|---|---|
| GC strategy | Tracing vs RC | RC with elision | Deterministic, lower memory, simpler |
| AST | Full AST vs single-pass | Single-pass | 15-20x less parse memory |
| Object model | Hidden classes vs tiers | 4-tier CDOL | No JIT to exploit hidden classes |
| String encoding | UTF-16 vs UTF-8 vs Latin-1 | UTF-8 + IS_ASCII | 91.5% ASCII, saves ~42% memory |
| Value encoding | Pointer tagging vs NaN-boxing | NaN-boxing | Zero-cost doubles, 8 bytes uniform |
| Regex | Backtrack vs NFA vs both | Tiered (both) | Simple patterns: BT. Complex: NFA |
| UI approach | WebKit vs custom vs native | Custom 4-layer | 1300x smaller than Electron |
| Security | Wasm vs seccomp vs both | 5-layer defense-in-depth | Kernel enforcement + JS-level ocap |
| Dispatch | Switch vs computed goto | Computed goto + fallback | ~15-20% faster, portable fallback |
| Superinstructions | 30 pairs vs 5-7 chains | 5-7 chains | Higher coverage, fewer opcodes |

---

END OF SPECIFICATION
