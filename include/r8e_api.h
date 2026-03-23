/*
 * r8e_api.h - Public API for the r8e JavaScript Engine
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 1 (System Overview) and Section 11 (Security).
 *
 * This is the primary header for embedding r8e. It provides:
 *   - Context creation and destruction
 *   - Script evaluation
 *   - Value creation, extraction, and type checking
 *   - Realm management
 *   - Error handling
 *   - Native function binding
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef R8E_API_H
#define R8E_API_H

#include "r8e_types.h"
#include "r8e_atoms.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Version Information
 * ========================================================================= */

#define R8E_VERSION_MAJOR  0
#define R8E_VERSION_MINOR  1
#define R8E_VERSION_PATCH  0
#define R8E_VERSION_STRING "0.1.0"

/* =========================================================================
 * Error Codes
 * ========================================================================= */

typedef enum {
    R8E_OK              =  0,   /* success */
    R8E_ERROR           = -1,   /* generic error */
    R8E_ERROR_SYNTAX    = -2,   /* JS syntax error */
    R8E_ERROR_TYPE      = -3,   /* JS type error */
    R8E_ERROR_RANGE     = -4,   /* JS range error */
    R8E_ERROR_REFERENCE = -5,   /* JS reference error */
    R8E_ERROR_URI       = -6,   /* JS URI error */
    R8E_ERROR_EVAL      = -7,   /* JS eval error */
    R8E_ERROR_INTERNAL  = -8,   /* internal engine error */
    R8E_ERROR_OOM       = -9,   /* out of memory */
    R8E_ERROR_STACK     = -10,  /* stack overflow */
    R8E_ERROR_LIMIT     = -11,  /* resource limit exceeded */
    R8E_ERROR_VERIFY    = -12,  /* bytecode verification failed */
} R8EStatus;


/* =========================================================================
 * Section 1: Context Lifecycle
 *
 * The R8EContext is the top-level engine state. All JS execution happens
 * within a context. A context owns one or more realms, the atom table,
 * and GC state.
 * ========================================================================= */

/**
 * Create a new r8e JavaScript engine context.
 *
 * The context is initialized with:
 *   - Pre-interned atom table (256+ common names)
 *   - Default realm with standard built-in prototypes
 *   - Default resource limits
 *   - GC state with epoch threshold = 4096
 *
 * @return  Newly allocated context, or NULL on out-of-memory.
 *          Must be freed with r8e_context_free().
 */
R8EContext *r8e_context_new(void);

/**
 * Destroy a context and free all associated memory.
 *
 * This frees:
 *   - All realms and their arenas
 *   - The atom table and all interned strings
 *   - All GC-tracked objects
 *   - The context struct itself
 *
 * After this call, the context pointer is invalid.
 *
 * @param ctx   Context to destroy (NULL is safe, does nothing).
 */
void r8e_context_free(R8EContext *ctx);


/* =========================================================================
 * Section 2: Script Evaluation
 * ========================================================================= */

/**
 * Evaluate a JavaScript source string.
 *
 * The source is compiled (single-pass, no AST) and executed immediately
 * in the current realm of the context.
 *
 * @param ctx      Engine context.
 * @param source   UTF-8 encoded JavaScript source code.
 * @param len      Length of source in bytes (0 = use strlen).
 * @return         The result value of the script, or R8E_UNDEFINED on error.
 *                 Check r8e_has_exception(ctx) for errors.
 */
R8EValue r8e_eval(R8EContext *ctx, const char *source, size_t len);

/**
 * Evaluate a JavaScript source string with a filename for error reporting.
 *
 * @param ctx       Engine context.
 * @param source    UTF-8 encoded JavaScript source code.
 * @param len       Length of source in bytes (0 = use strlen).
 * @param filename  Source filename for stack traces (not owned, must outlive eval).
 * @return          The result value of the script.
 */
R8EValue r8e_eval_file(R8EContext *ctx, const char *source, size_t len,
                       const char *filename);

/**
 * Compile a JavaScript source string to bytecode without executing it.
 *
 * @param ctx       Engine context.
 * @param source    UTF-8 encoded JavaScript source code.
 * @param len       Length of source in bytes (0 = use strlen).
 * @param filename  Source filename for error reporting (can be NULL).
 * @return          Compiled function object, or R8E_UNDEFINED on error.
 */
R8EValue r8e_compile(R8EContext *ctx, const char *source, size_t len,
                     const char *filename);

/**
 * Call a JavaScript function.
 *
 * @param ctx      Engine context.
 * @param func     The function value to call.
 * @param this_val The 'this' binding for the call.
 * @param argc     Number of arguments.
 * @param argv     Array of argument values (can be NULL if argc == 0).
 * @return         Return value of the function.
 */
R8EValue r8e_call(R8EContext *ctx, R8EValue func, R8EValue this_val,
                  int argc, const R8EValue *argv);


/* =========================================================================
 * Section 3: Value Creation
 * ========================================================================= */

/**
 * Create a number value from a double.
 * If the value is a safe integer, stores as int32 for efficiency.
 */
R8EValue r8e_make_number(double d);

/**
 * Create a string value from UTF-8 bytes.
 * The string is copied; caller retains ownership of the input.
 * Short ASCII strings (0-7 chars) may be stored inline (zero allocation).
 *
 * @param ctx  Engine context (for heap allocation if needed).
 * @param str  UTF-8 string data.
 * @param len  Length in bytes (0 = use strlen).
 * @return     String value, or R8E_UNDEFINED on OOM.
 */
R8EValue r8e_make_string(R8EContext *ctx, const char *str, size_t len);

/**
 * Create a string value from a C string (null-terminated).
 */
R8EValue r8e_make_cstring(R8EContext *ctx, const char *str);

/**
 * Create a new empty object.
 *
 * @param ctx  Engine context.
 * @return     New object value (Tier 0), or R8E_UNDEFINED on OOM.
 */
R8EValue r8e_make_object(R8EContext *ctx);

/**
 * Create a new array with the given initial capacity.
 *
 * @param ctx       Engine context.
 * @param capacity  Initial element capacity (0 = default).
 * @return          New array value, or R8E_UNDEFINED on OOM.
 */
R8EValue r8e_make_array(R8EContext *ctx, uint32_t capacity);


/* =========================================================================
 * Section 4: Value Type Checking
 *
 * All type checks are branchless single-comparison operations.
 * ========================================================================= */

/** Check if value is undefined. */
static inline bool r8e_is_undefined(R8EValue v) { return R8E_IS_UNDEFINED(v); }

/** Check if value is null. */
static inline bool r8e_is_null(R8EValue v) { return R8E_IS_NULL(v); }

/** Check if value is null or undefined. */
static inline bool r8e_is_nullish(R8EValue v) { return R8E_IS_NULLISH(v); }

/** Check if value is a boolean. */
static inline bool r8e_is_bool(R8EValue v) { return R8E_IS_BOOL(v); }

/** Check if value is a number (int32 or double). */
static inline bool r8e_is_number(R8EValue v) { return R8E_IS_NUMBER(v); }

/** Check if value is an int32. */
static inline bool r8e_is_int32(R8EValue v) { return R8E_IS_INT32(v); }

/** Check if value is a double. */
static inline bool r8e_is_double(R8EValue v) { return R8E_IS_DOUBLE(v); }

/** Check if value is an inline short string. */
static inline bool r8e_is_inline_str(R8EValue v) { return R8E_IS_INLINE_STR(v); }

/** Check if value is a heap pointer (object, string, closure, etc.). */
static inline bool r8e_is_object(R8EValue v) { return R8E_IS_POINTER(v); }

/** Check if value is a symbol. */
static inline bool r8e_is_symbol(R8EValue v) { return R8E_IS_SYMBOL(v); }

/**
 * Check if value is a string (inline short or heap string).
 * For heap strings, dereferences the pointer to check the GC kind.
 */
bool r8e_is_string(R8EValue v);

/**
 * Check if value is a function (closure or native function).
 */
bool r8e_is_function(R8EValue v);

/**
 * Check if value is an array.
 */
bool r8e_is_array(R8EValue v);


/* =========================================================================
 * Section 5: Value Extraction
 * ========================================================================= */

/**
 * Convert a value to a C double.
 * Returns 0.0 for non-numeric values (use r8e_to_number for full conversion).
 */
static inline double r8e_to_double(R8EValue v) {
    if (R8E_IS_DOUBLE(v)) return r8e_get_double(v);
    if (R8E_IS_INT32(v)) return (double)r8e_get_int32(v);
    return 0.0;
}

/**
 * Convert a value to a C int32.
 * Truncates doubles. Returns 0 for non-numeric values.
 */
static inline int32_t r8e_to_int32(R8EValue v) {
    if (R8E_IS_INT32(v)) return r8e_get_int32(v);
    if (R8E_IS_DOUBLE(v)) return (int32_t)r8e_get_double(v);
    return 0;
}

/**
 * Convert a value to a boolean (JS truthiness rules).
 */
bool r8e_to_bool(R8EValue v);

/**
 * Convert a value to a string.
 * Returns a new string value. Caller must manage RC.
 */
R8EValue r8e_to_string(R8EContext *ctx, R8EValue v);

/**
 * Get the C string pointer and length from a string value.
 * For inline strings, writes to the provided buffer (must be >= 8 bytes).
 * For heap strings, returns pointer to internal data (valid until GC).
 *
 * @param v      String value (inline or heap).
 * @param buf    Buffer for inline string decoding (>= 8 bytes).
 * @param outlen Output: string length in bytes.
 * @return       Pointer to UTF-8 string data.
 */
const char *r8e_get_cstring(R8EValue v, char *buf, size_t *outlen);


/* =========================================================================
 * Section 6: Object Property Access
 * ========================================================================= */

/**
 * Get a property by name.
 *
 * @param ctx   Engine context.
 * @param obj   Object value.
 * @param name  Property name (C string).
 * @return      Property value, or R8E_UNDEFINED if not found.
 */
R8EValue r8e_get_prop(R8EContext *ctx, R8EValue obj, const char *name);

/**
 * Set a property by name.
 *
 * @param ctx   Engine context.
 * @param obj   Object value.
 * @param name  Property name (C string).
 * @param val   Value to set.
 * @return      R8E_OK on success, error code on failure.
 */
R8EStatus r8e_set_prop(R8EContext *ctx, R8EValue obj, const char *name,
                       R8EValue val);

/**
 * Define an accessor property (getter/setter) on an object.
 *
 * When get_prop is called on this property, the getter function is invoked.
 * When set_prop is called, the setter function is invoked.
 *
 * @param ctx     Engine context.
 * @param obj     Object value.
 * @param name    Property name (C string).
 * @param getter  Getter function value (R8E_UNDEFINED for no getter).
 * @param setter  Setter function value (R8E_UNDEFINED for no setter).
 * @return        R8E_OK on success, error code on failure.
 */
R8EStatus r8e_define_accessor(R8EContext *ctx, R8EValue obj, const char *name,
                                R8EValue getter, R8EValue setter);

/**
 * Delete a property by name.
 *
 * @param ctx   Engine context.
 * @param obj   Object value.
 * @param name  Property name (C string).
 * @return      true if property was deleted, false otherwise.
 */
bool r8e_delete_prop(R8EContext *ctx, R8EValue obj, const char *name);

/**
 * Check if an object has a property (own, not inherited).
 */
bool r8e_has_prop(R8EContext *ctx, R8EValue obj, const char *name);

/**
 * Get a property by atom index (faster than by name, no interning needed).
 */
R8EValue r8e_get_prop_atom(R8EContext *ctx, R8EValue obj, uint32_t atom);

/**
 * Set a property by atom index.
 */
R8EStatus r8e_set_prop_atom(R8EContext *ctx, R8EValue obj, uint32_t atom,
                            R8EValue val);

/**
 * Get an array element by index.
 */
R8EValue r8e_get_element(R8EContext *ctx, R8EValue arr, uint32_t index);

/**
 * Set an array element by index.
 */
R8EStatus r8e_set_element(R8EContext *ctx, R8EValue arr, uint32_t index,
                          R8EValue val);

/**
 * Get the length of an array or string.
 * Returns -1 if the value is not an array or string.
 */
int32_t r8e_get_length(R8EContext *ctx, R8EValue v);


/* =========================================================================
 * Section 7: Native Function Binding
 * ========================================================================= */

/**
 * C function callback type.
 *
 * @param ctx      Engine context.
 * @param this_val The 'this' binding.
 * @param argc     Number of arguments.
 * @param argv     Argument values.
 * @return         Return value for the JS caller.
 */
typedef R8EValue (*R8ENativeFunc)(R8EContext *ctx, R8EValue this_val,
                                  int argc, const R8EValue *argv);

/**
 * Create a native function value.
 *
 * @param ctx    Engine context.
 * @param func   C function pointer.
 * @param name   Function name (for stack traces).
 * @param argc   Expected argument count (-1 for variadic).
 * @return       Function value, or R8E_UNDEFINED on OOM.
 */
R8EValue r8e_make_native_func(R8EContext *ctx, R8ENativeFunc func,
                               const char *name, int argc);

/**
 * Set a global function.
 * Convenience wrapper: creates a native function and sets it as a global property.
 *
 * @param ctx    Engine context.
 * @param name   Global function name.
 * @param func   C function pointer.
 * @param argc   Expected argument count.
 * @return       R8E_OK on success.
 */
R8EStatus r8e_set_global_func(R8EContext *ctx, const char *name,
                               R8ENativeFunc func, int argc);

/**
 * Set a global value.
 *
 * @param ctx   Engine context.
 * @param name  Global property name.
 * @param val   Value to set.
 * @return      R8E_OK on success.
 */
R8EStatus r8e_set_global(R8EContext *ctx, const char *name, R8EValue val);

/**
 * Get a global value.
 *
 * @param ctx   Engine context.
 * @param name  Global property name.
 * @return      Global value, or R8E_UNDEFINED if not found.
 */
R8EValue r8e_get_global(R8EContext *ctx, const char *name);


/* =========================================================================
 * Section 8: Error Handling
 * ========================================================================= */

/**
 * Check if the context has a pending exception.
 */
static inline bool r8e_has_exception(const R8EContext *ctx) {
    return ctx->error.has_exception;
}

/**
 * Get the pending exception value.
 * Returns R8E_UNDEFINED if no exception is pending.
 */
static inline R8EValue r8e_get_exception(const R8EContext *ctx) {
    return ctx->error.has_exception ? ctx->error.exception : R8E_UNDEFINED;
}

/**
 * Clear the pending exception.
 * Returns the exception value that was cleared.
 */
R8EValue r8e_clear_exception(R8EContext *ctx);

/**
 * Throw a TypeError with the given message.
 */
R8EValue r8e_throw_type_error(R8EContext *ctx, const char *fmt, ...);

/**
 * Throw a RangeError with the given message.
 */
R8EValue r8e_throw_range_error(R8EContext *ctx, const char *fmt, ...);

/**
 * Throw a SyntaxError with the given message.
 */
R8EValue r8e_throw_syntax_error(R8EContext *ctx, const char *fmt, ...);

/**
 * Throw a ReferenceError with the given message.
 */
R8EValue r8e_throw_reference_error(R8EContext *ctx, const char *fmt, ...);

/**
 * Throw a generic Error with the given message.
 */
R8EValue r8e_throw_error(R8EContext *ctx, const char *fmt, ...);


/* =========================================================================
 * Section 9: Realm Management (CLAUDE.md Section 11.6)
 *
 * Realms are lightweight V8-Isolate equivalents: ~64KB each.
 * Each has isolated memory, prototypes, and globals.
 * ========================================================================= */

/**
 * Create a new realm within the context.
 *
 * @param ctx  Engine context.
 * @return     Realm index (0-15), or -1 on error (max realms reached or OOM).
 */
int r8e_realm_new(R8EContext *ctx);

/**
 * Switch the active realm.
 *
 * @param ctx        Engine context.
 * @param realm_id   Realm index to activate.
 * @return           Previous realm index, or -1 on error.
 */
int r8e_realm_switch(R8EContext *ctx, int realm_id);

/**
 * Get the current active realm index.
 */
static inline int r8e_realm_current(const R8EContext *ctx) {
    return ctx->current_realm;
}

/**
 * Set resource limits on a realm.
 *
 * @param ctx        Engine context.
 * @param realm_id   Realm index.
 * @param limits     Resource limits to apply.
 * @return           R8E_OK on success.
 */
R8EStatus r8e_realm_set_limits(R8EContext *ctx, int realm_id,
                                const R8ERealmLimits *limits);

/**
 * Destroy a realm and free its memory.
 * Cannot destroy realm 0 (the default realm).
 *
 * @param ctx        Engine context.
 * @param realm_id   Realm index to destroy.
 * @return           R8E_OK on success, R8E_ERROR on invalid realm.
 */
R8EStatus r8e_realm_destroy(R8EContext *ctx, int realm_id);


/* =========================================================================
 * Section 10: GC Control
 * ========================================================================= */

/**
 * Force a cycle detection scan.
 * Normally happens automatically when suspect list exceeds threshold.
 */
void r8e_gc_collect(R8EContext *ctx);

/**
 * Get current memory usage statistics.
 */
typedef struct {
    uint64_t total_allocated;    /* total bytes allocated */
    uint64_t total_freed;        /* total bytes freed */
    uint64_t current_usage;      /* current live bytes (allocated - freed) */
    uint32_t suspect_count;      /* objects on suspect list */
    uint32_t epoch_threshold;    /* current epoch threshold */
    uint32_t object_count;       /* approximate live object count */
} R8EMemStats;

void r8e_gc_stats(const R8EContext *ctx, R8EMemStats *stats);

/**
 * Increment reference count of a value.
 * Only needed for heap-allocated values (pointers).
 * No-op for non-pointer values.
 */
void r8e_value_retain(R8EValue v);

/**
 * Decrement reference count of a value.
 * May free the value if refcount reaches zero.
 * No-op for non-pointer values.
 */
void r8e_value_release(R8EContext *ctx, R8EValue v);


/* =========================================================================
 * Section 11: Module System
 * ========================================================================= */

/**
 * Module loader callback type.
 * Called when an import statement needs to resolve a module specifier.
 *
 * @param ctx        Engine context.
 * @param specifier  The module specifier string (e.g., "./foo.js").
 * @param referrer   The module that contains the import (can be NULL).
 * @param out_source Output: module source code (must be allocated with r8e_alloc).
 * @param out_len    Output: source code length.
 * @return           R8E_OK on success, error code on failure.
 */
typedef R8EStatus (*R8EModuleLoader)(R8EContext *ctx, const char *specifier,
                                      const char *referrer, char **out_source,
                                      size_t *out_len);

/**
 * Set the module loader callback.
 */
void r8e_set_module_loader(R8EContext *ctx, R8EModuleLoader loader);

/**
 * Load and evaluate a module.
 *
 * @param ctx       Engine context.
 * @param source    Module source code.
 * @param len       Source length in bytes.
 * @param filename  Module filename/URL for resolution.
 * @return          Module namespace object, or R8E_UNDEFINED on error.
 */
R8EValue r8e_eval_module(R8EContext *ctx, const char *source, size_t len,
                          const char *filename);


/* =========================================================================
 * Section 12: Memory Allocation
 *
 * Custom allocators for embedding. The context uses these for all
 * internal allocations.
 * ========================================================================= */

/**
 * Custom allocator function types.
 */
typedef void *(*R8EMallocFunc)(size_t size, void *user_data);
typedef void *(*R8EReallocFunc)(void *ptr, size_t size, void *user_data);
typedef void  (*R8EFreeFunc)(void *ptr, void *user_data);

typedef struct {
    R8EMallocFunc   malloc_func;
    R8EReallocFunc  realloc_func;
    R8EFreeFunc     free_func;
    void           *user_data;
} R8EAllocator;

/**
 * Create a context with a custom allocator.
 */
R8EContext *r8e_context_new_alloc(const R8EAllocator *alloc);


/* =========================================================================
 * Section 13: Utility
 * ========================================================================= */

/**
 * Get the r8e engine version string.
 */
static inline const char *r8e_version(void) {
    return R8E_VERSION_STRING;
}

/**
 * Dump bytecode for debugging (writes to stderr).
 */
void r8e_dump_bytecode(const R8EFunction *func);

/**
 * Dump value for debugging (writes to stderr).
 */
void r8e_dump_value(R8EValue v);


#ifdef __cplusplus
}
#endif

#endif /* R8E_API_H */
