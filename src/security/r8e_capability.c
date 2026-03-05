/*
 * r8e_capability.c - Layer 5: Capability-Based API Surface
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 11.7 (Layer 5: Capability-Based API Surface).
 *
 * Architecture:
 *   - Object-capability model (ocap): no ambient authority
 *   - Code cannot use capabilities it does not have a reference to
 *   - NO global fs/net/process APIs
 *   - All OS interaction through explicit capability objects
 *   - Capabilities can be attenuated (narrowed) before passing
 *     to less-trusted code
 *   - Capabilities can be revoked, permanently disabling them
 *   - Timer resolution clamping: 1ms minimum (Section 11.8)
 *
 * Capability types:
 *   - fs:      filesystem access (read/write/create, root restriction, quota)
 *   - net:     network access (allowed hosts/ports, protocol restriction)
 *   - timer:   setTimeout/setInterval (resolution clamping)
 *   - env:     environment variable access
 *   - crypto:  cryptographic random number generation
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../include/r8e_types.h"
#include "../../include/r8e_api.h"

/* =========================================================================
 * Capability Type IDs
 * ========================================================================= */

typedef enum {
    R8E_CAP_FS       = 0,   /* filesystem access */
    R8E_CAP_NET      = 1,   /* network access */
    R8E_CAP_TIMER    = 2,   /* setTimeout/setInterval */
    R8E_CAP_ENV      = 3,   /* environment variables */
    R8E_CAP_CRYPTO   = 4,   /* cryptographic operations */
    R8E_CAP_CUSTOM   = 5,   /* user-defined capability */
    R8E_CAP_COUNT    = 6
} R8ECapType;

/* =========================================================================
 * Permission Flags (per capability type)
 * ========================================================================= */

/* Filesystem permissions */
#define R8E_PERM_FS_READ     0x0001U  /* read files */
#define R8E_PERM_FS_WRITE    0x0002U  /* write/overwrite files */
#define R8E_PERM_FS_CREATE   0x0004U  /* create new files */
#define R8E_PERM_FS_DELETE   0x0008U  /* delete files */
#define R8E_PERM_FS_LIST     0x0010U  /* list directories */
#define R8E_PERM_FS_STAT     0x0020U  /* stat/access check */
#define R8E_PERM_FS_ALL      0x003FU

/* Network permissions */
#define R8E_PERM_NET_CONNECT  0x0001U  /* outbound connections */
#define R8E_PERM_NET_LISTEN   0x0002U  /* listen for inbound */
#define R8E_PERM_NET_DNS      0x0004U  /* DNS resolution */
#define R8E_PERM_NET_TCP      0x0010U  /* TCP protocol */
#define R8E_PERM_NET_UDP      0x0020U  /* UDP protocol */
#define R8E_PERM_NET_HTTP     0x0040U  /* HTTP/HTTPS */
#define R8E_PERM_NET_WS       0x0080U  /* WebSocket */
#define R8E_PERM_NET_ALL      0x00F7U

/* Timer permissions */
#define R8E_PERM_TIMER_SET     0x0001U  /* create timers */
#define R8E_PERM_TIMER_CLEAR   0x0002U  /* cancel timers */
#define R8E_PERM_TIMER_ALL     0x0003U

/* Environment permissions */
#define R8E_PERM_ENV_READ      0x0001U  /* read env vars */
#define R8E_PERM_ENV_ALL       0x0001U

/* Crypto permissions */
#define R8E_PERM_CRYPTO_RANDOM 0x0001U  /* getRandomValues */
#define R8E_PERM_CRYPTO_DIGEST 0x0002U  /* hash functions */
#define R8E_PERM_CRYPTO_ALL    0x0003U

/* =========================================================================
 * Capability Structures
 * ========================================================================= */

/* Maximum path/host entries per capability */
#define R8E_CAP_MAX_PATHS    16
#define R8E_CAP_MAX_HOSTS    16
#define R8E_CAP_MAX_PATH_LEN 256

/* Filesystem capability configuration */
typedef struct {
    char     root[R8E_CAP_MAX_PATH_LEN]; /* root directory restriction */
    uint32_t permissions;                /* R8E_PERM_FS_* bitmask */
    size_t   quota_bytes;                /* max bytes writable (0 = unlimited) */
    size_t   bytes_written;              /* tracking: bytes written so far */
} R8ECapFS;

/* Network capability configuration */
typedef struct {
    char     allowed_hosts[R8E_CAP_MAX_HOSTS][256]; /* hostname patterns */
    uint16_t allowed_ports[R8E_CAP_MAX_HOSTS];      /* port restrictions */
    uint8_t  host_count;
    uint32_t permissions;                /* R8E_PERM_NET_* bitmask */
} R8ECapNet;

/* Timer capability configuration */
typedef struct {
    uint32_t permissions;          /* R8E_PERM_TIMER_* bitmask */
    uint32_t min_interval_ms;      /* minimum timer interval (default 1ms) */
    uint32_t max_concurrent;       /* max concurrent timers (0 = unlimited) */
    uint32_t current_count;        /* tracking: active timers */
} R8ECapTimer;

/* Environment capability configuration */
typedef struct {
    uint32_t permissions;          /* R8E_PERM_ENV_* bitmask */
    char     allowed_vars[16][64]; /* allowed variable name patterns */
    uint8_t  var_count;
} R8ECapEnv;

/* Crypto capability configuration */
typedef struct {
    uint32_t permissions;          /* R8E_PERM_CRYPTO_* bitmask */
} R8ECapCrypto;

/* =========================================================================
 * Main Capability Object
 *
 * This is the core data structure representing a capability. It is
 * reference-counted and can be attenuated (narrowed) or revoked.
 *
 * IMPORTANT: Once a capability is revoked, ALL operations through it
 * fail immediately. Revocation is permanent and cannot be undone.
 * ========================================================================= */

#define R8E_CAP_MAGIC 0x52384543U  /* "R8EC" */

typedef struct R8ECapability {
    uint32_t         magic;         /* R8E_CAP_MAGIC (for validation) */
    R8ECapType       type;          /* capability type */
    uint32_t         id;            /* unique ID */
    uint32_t         realm_id;      /* owning realm */
    bool             revoked;       /* true = permanently disabled */
    uint32_t         refcount;      /* reference count */

    /* Parent capability (for attenuation chains) */
    struct R8ECapability *parent;

    /* Type-specific configuration */
    union {
        R8ECapFS     fs;
        R8ECapNet    net;
        R8ECapTimer  timer;
        R8ECapEnv    env;
        R8ECapCrypto crypto;
    } config;
} R8ECapability;

/* Global capability ID counter */
static uint32_t g_cap_next_id = 1;

/* =========================================================================
 * Capability Validation
 *
 * Security-critical: every capability use goes through validation.
 * ========================================================================= */

/**
 * Validate that a pointer is a legitimate capability object.
 * Checks the magic number and basic structural integrity.
 *
 * @param cap  Pointer to validate.
 * @return     true if valid, false if corrupted or fake.
 */
static bool cap_validate(const R8ECapability *cap) {
    if (!cap) return false;
    if (cap->magic != R8E_CAP_MAGIC) return false;
    if (cap->type >= R8E_CAP_COUNT) return false;
    return true;
}

/**
 * Check if a capability is currently usable.
 * A capability is usable if it is valid and not revoked.
 * Also checks the parent chain: if any ancestor is revoked,
 * this capability is also effectively revoked.
 *
 * @param cap  Capability to check.
 * @return     true if usable, false if revoked or invalid.
 */
static bool cap_is_usable(const R8ECapability *cap) {
    /* Walk the attenuation chain to check for revocation */
    const R8ECapability *c = cap;
    int chain_depth = 0;
    while (c) {
        if (!cap_validate(c)) return false;
        if (c->revoked) return false;
        c = c->parent;
        chain_depth++;
        /* Guard against corrupted circular chains */
        if (chain_depth > 100) return false;
    }
    return true;
}

/* =========================================================================
 * Capability Creation
 * ========================================================================= */

/**
 * Create a new capability object.
 *
 * @param type      Capability type (fs, net, timer, etc.).
 * @param realm_id  Owning realm ID.
 * @return          New capability, or NULL on failure.
 */
static R8ECapability *cap_alloc(R8ECapType type, uint32_t realm_id) {
    R8ECapability *cap = (R8ECapability *)calloc(1, sizeof(R8ECapability));
    if (!cap) return NULL;

    cap->magic = R8E_CAP_MAGIC;
    cap->type = type;
    cap->id = g_cap_next_id++;
    cap->realm_id = realm_id;
    cap->revoked = false;
    cap->refcount = 1;
    cap->parent = NULL;

    return cap;
}

/**
 * Create a filesystem capability.
 *
 * @param ctx          Engine context.
 * @param root         Root directory (absolute path). All operations are
 *                     restricted to this subtree.
 * @param permissions  Bitmask of R8E_PERM_FS_* flags.
 * @param quota_bytes  Maximum total bytes writable (0 = unlimited).
 * @return             Capability value, or R8E_UNDEFINED on error.
 */
R8EValue r8e_capability_create_fs(R8EContext *ctx, const char *root,
                                   uint32_t permissions, size_t quota_bytes)
{
    if (!ctx) return R8E_UNDEFINED;

    R8ECapability *cap = cap_alloc(R8E_CAP_FS, ctx->current_realm);
    if (!cap) return R8E_UNDEFINED;

    /* Normalize and store root path */
    if (root && root[0] != '\0') {
        size_t rlen = strlen(root);
        if (rlen >= R8E_CAP_MAX_PATH_LEN) {
            rlen = R8E_CAP_MAX_PATH_LEN - 1;
        }
        memcpy(cap->config.fs.root, root, rlen);
        cap->config.fs.root[rlen] = '\0';

        /* Strip trailing slash (unless root is "/") */
        if (rlen > 1 && cap->config.fs.root[rlen - 1] == '/') {
            cap->config.fs.root[rlen - 1] = '\0';
        }
    }

    cap->config.fs.permissions = permissions & R8E_PERM_FS_ALL;
    cap->config.fs.quota_bytes = quota_bytes;
    cap->config.fs.bytes_written = 0;

    return r8e_from_pointer(cap);
}

/**
 * Create a network capability.
 *
 * @param ctx          Engine context.
 * @param permissions  Bitmask of R8E_PERM_NET_* flags.
 * @return             Capability value, or R8E_UNDEFINED on error.
 */
R8EValue r8e_capability_create_net(R8EContext *ctx, uint32_t permissions) {
    if (!ctx) return R8E_UNDEFINED;

    R8ECapability *cap = cap_alloc(R8E_CAP_NET, ctx->current_realm);
    if (!cap) return R8E_UNDEFINED;

    cap->config.net.permissions = permissions & R8E_PERM_NET_ALL;
    cap->config.net.host_count = 0;

    return r8e_from_pointer(cap);
}

/**
 * Add an allowed host to a network capability.
 *
 * @param cap_val  Network capability value.
 * @param host     Hostname pattern (e.g. "*.example.com").
 * @param port     Allowed port (0 = any).
 * @return         R8E_OK on success.
 */
R8EStatus r8e_capability_net_add_host(R8EValue cap_val,
                                       const char *host, uint16_t port)
{
    if (!R8E_IS_POINTER(cap_val) || !host) return R8E_ERROR;

    R8ECapability *cap = (R8ECapability *)r8e_get_pointer(cap_val);
    if (!cap_validate(cap) || cap->type != R8E_CAP_NET) return R8E_ERROR;
    if (cap->revoked) return R8E_ERROR;

    if (cap->config.net.host_count >= R8E_CAP_MAX_HOSTS) return R8E_ERROR;

    uint8_t idx = cap->config.net.host_count;
    size_t hlen = strlen(host);
    if (hlen >= sizeof(cap->config.net.allowed_hosts[0])) {
        hlen = sizeof(cap->config.net.allowed_hosts[0]) - 1;
    }
    memcpy(cap->config.net.allowed_hosts[idx], host, hlen);
    cap->config.net.allowed_hosts[idx][hlen] = '\0';
    cap->config.net.allowed_ports[idx] = port;
    cap->config.net.host_count++;

    return R8E_OK;
}

/**
 * Create a timer capability.
 *
 * @param ctx              Engine context.
 * @param min_interval_ms  Minimum timer interval in milliseconds.
 *                         Must be >= 1 (clamped to prevent timing attacks).
 * @param max_concurrent   Maximum concurrent timers (0 = unlimited).
 * @return                 Capability value, or R8E_UNDEFINED on error.
 */
R8EValue r8e_capability_create_timer(R8EContext *ctx,
                                      uint32_t min_interval_ms,
                                      uint32_t max_concurrent)
{
    if (!ctx) return R8E_UNDEFINED;

    R8ECapability *cap = cap_alloc(R8E_CAP_TIMER, ctx->current_realm);
    if (!cap) return R8E_UNDEFINED;

    cap->config.timer.permissions = R8E_PERM_TIMER_ALL;
    /* Enforce minimum 1ms interval (Section 11.8: timer resolution clamping) */
    cap->config.timer.min_interval_ms = (min_interval_ms < 1) ? 1 : min_interval_ms;
    cap->config.timer.max_concurrent = max_concurrent;
    cap->config.timer.current_count = 0;

    return r8e_from_pointer(cap);
}

/**
 * Create an environment variable capability.
 *
 * @param ctx  Engine context.
 * @return     Capability value, or R8E_UNDEFINED on error.
 */
R8EValue r8e_capability_create_env(R8EContext *ctx) {
    if (!ctx) return R8E_UNDEFINED;

    R8ECapability *cap = cap_alloc(R8E_CAP_ENV, ctx->current_realm);
    if (!cap) return R8E_UNDEFINED;

    cap->config.env.permissions = R8E_PERM_ENV_READ;
    cap->config.env.var_count = 0;

    return r8e_from_pointer(cap);
}

/**
 * Add an allowed environment variable pattern.
 */
R8EStatus r8e_capability_env_add_var(R8EValue cap_val, const char *pattern) {
    if (!R8E_IS_POINTER(cap_val) || !pattern) return R8E_ERROR;

    R8ECapability *cap = (R8ECapability *)r8e_get_pointer(cap_val);
    if (!cap_validate(cap) || cap->type != R8E_CAP_ENV) return R8E_ERROR;
    if (cap->revoked) return R8E_ERROR;

    if (cap->config.env.var_count >= 16) return R8E_ERROR;

    uint8_t idx = cap->config.env.var_count;
    size_t plen = strlen(pattern);
    if (plen >= sizeof(cap->config.env.allowed_vars[0])) {
        plen = sizeof(cap->config.env.allowed_vars[0]) - 1;
    }
    memcpy(cap->config.env.allowed_vars[idx], pattern, plen);
    cap->config.env.allowed_vars[idx][plen] = '\0';
    cap->config.env.var_count++;

    return R8E_OK;
}

/**
 * Create a crypto capability.
 *
 * @param ctx          Engine context.
 * @param permissions  Bitmask of R8E_PERM_CRYPTO_* flags.
 * @return             Capability value, or R8E_UNDEFINED on error.
 */
R8EValue r8e_capability_create_crypto(R8EContext *ctx, uint32_t permissions) {
    if (!ctx) return R8E_UNDEFINED;

    R8ECapability *cap = cap_alloc(R8E_CAP_CRYPTO, ctx->current_realm);
    if (!cap) return R8E_UNDEFINED;

    cap->config.crypto.permissions = permissions & R8E_PERM_CRYPTO_ALL;

    return r8e_from_pointer(cap);
}

/* =========================================================================
 * Capability Attenuation
 *
 * Create a restricted version of an existing capability. The new capability
 * can only do a SUBSET of what the parent allows. It forms a chain:
 * if the parent is revoked, the attenuated child is also revoked.
 * ========================================================================= */

/**
 * Path containment check.
 * Verify that child_path is a subdirectory of parent_path.
 * Prevents path traversal attacks (e.g., "../../etc/passwd").
 *
 * @param parent_path  The allowed root directory.
 * @param child_path   The requested subdirectory.
 * @return             true if child_path is within parent_path.
 */
static bool path_is_contained(const char *parent_path, const char *child_path) {
    if (!parent_path || !child_path) return false;

    size_t parent_len = strlen(parent_path);
    size_t child_len = strlen(child_path);

    /* Child must be at least as long as parent */
    if (child_len < parent_len) return false;

    /* Child must start with parent's path */
    if (memcmp(parent_path, child_path, parent_len) != 0) return false;

    /* If same length, they are the same path (OK) */
    if (child_len == parent_len) return true;

    /* The next character in child must be '/' (not a partial match) */
    if (child_path[parent_len] != '/') return false;

    /* Check for path traversal components ("../", "..") */
    const char *p = child_path;
    while (*p) {
        if (p[0] == '.' && p[1] == '.') {
            if (p[2] == '/' || p[2] == '\0') {
                return false; /* traversal detected */
            }
        }
        /* Advance to next path component */
        while (*p && *p != '/') p++;
        while (*p == '/') p++;
    }

    return true;
}

/**
 * Simple wildcard hostname matching.
 * Supports patterns like "*.example.com".
 *
 * @param pattern  Hostname pattern.
 * @param host     Actual hostname.
 * @return         true if host matches pattern.
 */
static bool host_matches(const char *pattern, const char *host) {
    if (!pattern || !host) return false;

    /* Exact match */
    if (strcmp(pattern, host) == 0) return true;

    /* Wildcard match: "*.example.com" matches "foo.example.com" */
    if (pattern[0] == '*' && pattern[1] == '.') {
        const char *suffix = pattern + 1; /* ".example.com" */
        size_t suffix_len = strlen(suffix);
        size_t host_len = strlen(host);
        if (host_len > suffix_len) {
            if (memcmp(host + host_len - suffix_len, suffix, suffix_len) == 0) {
                return true;
            }
        }
    }

    return false;
}

/**
 * Attenuate (narrow) a capability to create a more restricted version.
 *
 * The attenuated capability can only do things the parent allows.
 * If the parent is revoked, the attenuated child becomes unusable too.
 *
 * For filesystem capabilities: the new root must be a subdirectory of
 * the parent's root. The new permissions must be a subset.
 *
 * @param cap_val       The parent capability value.
 * @param new_root      New root directory (for fs caps; NULL to keep parent's).
 * @param new_perms     New permission bitmask (will be intersected with parent's).
 * @param new_quota     New quota (must be <= parent's quota).
 * @return              Attenuated capability value, or R8E_UNDEFINED on error.
 */
R8EValue r8e_capability_attenuate(R8EValue cap_val,
                                   const char *new_root,
                                   uint32_t new_perms,
                                   size_t new_quota)
{
    if (!R8E_IS_POINTER(cap_val)) return R8E_UNDEFINED;

    R8ECapability *parent = (R8ECapability *)r8e_get_pointer(cap_val);
    if (!cap_is_usable(parent)) return R8E_UNDEFINED;

    R8ECapability *child = cap_alloc(parent->type, parent->realm_id);
    if (!child) return R8E_UNDEFINED;

    /* Link to parent (for revocation propagation) */
    child->parent = parent;
    parent->refcount++;

    /* Type-specific attenuation */
    switch (parent->type) {
    case R8E_CAP_FS:
    {
        /* Copy parent config */
        child->config.fs = parent->config.fs;

        /* Narrow root directory */
        if (new_root && new_root[0] != '\0') {
            if (!path_is_contained(parent->config.fs.root, new_root)) {
                /* Child root is not within parent's root: reject */
                free(child);
                return R8E_UNDEFINED;
            }
            size_t rlen = strlen(new_root);
            if (rlen >= R8E_CAP_MAX_PATH_LEN) rlen = R8E_CAP_MAX_PATH_LEN - 1;
            memcpy(child->config.fs.root, new_root, rlen);
            child->config.fs.root[rlen] = '\0';
        }

        /* Narrow permissions (intersection only) */
        child->config.fs.permissions =
            parent->config.fs.permissions & new_perms;

        /* Narrow quota */
        if (new_quota > 0 && (parent->config.fs.quota_bytes == 0 ||
                              new_quota <= parent->config.fs.quota_bytes)) {
            child->config.fs.quota_bytes = new_quota;
        }

        /* Reset write tracking */
        child->config.fs.bytes_written = 0;
        break;
    }

    case R8E_CAP_NET:
    {
        child->config.net = parent->config.net;
        child->config.net.permissions =
            parent->config.net.permissions & new_perms;
        break;
    }

    case R8E_CAP_TIMER:
    {
        child->config.timer = parent->config.timer;
        child->config.timer.permissions =
            parent->config.timer.permissions & new_perms;
        /* Minimum interval can only increase (more restrictive) */
        if (new_perms > child->config.timer.min_interval_ms) {
            child->config.timer.min_interval_ms = new_perms;
        }
        break;
    }

    case R8E_CAP_ENV:
    {
        child->config.env = parent->config.env;
        child->config.env.permissions =
            parent->config.env.permissions & new_perms;
        break;
    }

    case R8E_CAP_CRYPTO:
    {
        child->config.crypto = parent->config.crypto;
        child->config.crypto.permissions =
            parent->config.crypto.permissions & new_perms;
        break;
    }

    default:
        free(child);
        return R8E_UNDEFINED;
    }

    return r8e_from_pointer(child);
}

/* =========================================================================
 * Capability Checking
 *
 * These functions are called by built-in native functions to verify
 * that the JS code has the appropriate capability for the operation
 * it is attempting.
 * ========================================================================= */

/**
 * Check if a filesystem operation is allowed.
 *
 * @param cap_val    Filesystem capability value.
 * @param path       Path being accessed (absolute).
 * @param operation  R8E_PERM_FS_* flag for the specific operation.
 * @param size       Size of data being written (for quota tracking).
 * @return           true if allowed, false if denied.
 */
bool r8e_capability_check_fs(R8EValue cap_val, const char *path,
                              uint32_t operation, size_t size)
{
    if (!R8E_IS_POINTER(cap_val) || !path) return false;

    R8ECapability *cap = (R8ECapability *)r8e_get_pointer(cap_val);
    if (!cap_is_usable(cap)) return false;
    if (cap->type != R8E_CAP_FS) return false;

    R8ECapFS *fs = &cap->config.fs;

    /* Check permission bit */
    if (!(fs->permissions & operation)) return false;

    /* Check path containment */
    if (fs->root[0] != '\0') {
        if (!path_is_contained(fs->root, path)) {
            return false; /* path is outside allowed root */
        }
    }

    /* Check write quota */
    if ((operation & (R8E_PERM_FS_WRITE | R8E_PERM_FS_CREATE)) &&
        fs->quota_bytes > 0) {
        if (fs->bytes_written + size > fs->quota_bytes) {
            return false; /* would exceed quota */
        }
    }

    return true;
}

/**
 * Record bytes written (for quota tracking).
 * Call AFTER a successful write operation.
 *
 * @param cap_val  Filesystem capability.
 * @param bytes    Number of bytes written.
 */
void r8e_capability_fs_record_write(R8EValue cap_val, size_t bytes) {
    if (!R8E_IS_POINTER(cap_val)) return;
    R8ECapability *cap = (R8ECapability *)r8e_get_pointer(cap_val);
    if (!cap_validate(cap) || cap->type != R8E_CAP_FS) return;
    cap->config.fs.bytes_written += bytes;
}

/**
 * Check if a network operation is allowed.
 *
 * @param cap_val    Network capability value.
 * @param host       Target hostname.
 * @param port       Target port.
 * @param operation  R8E_PERM_NET_* flag for the specific operation.
 * @return           true if allowed, false if denied.
 */
bool r8e_capability_check_net(R8EValue cap_val, const char *host,
                               uint16_t port, uint32_t operation)
{
    if (!R8E_IS_POINTER(cap_val)) return false;

    R8ECapability *cap = (R8ECapability *)r8e_get_pointer(cap_val);
    if (!cap_is_usable(cap)) return false;
    if (cap->type != R8E_CAP_NET) return false;

    R8ECapNet *net = &cap->config.net;

    /* Check operation permission */
    if (!(net->permissions & operation)) return false;

    /* If no hosts are configured, all hosts are allowed
     * (subject to the operation permission check above) */
    if (net->host_count == 0) return true;

    /* Check against allowed hosts */
    if (host) {
        bool host_allowed = false;
        for (uint8_t i = 0; i < net->host_count; i++) {
            if (host_matches(net->allowed_hosts[i], host)) {
                /* Check port restriction */
                if (net->allowed_ports[i] == 0 ||
                    net->allowed_ports[i] == port) {
                    host_allowed = true;
                    break;
                }
            }
        }
        if (!host_allowed) return false;
    }

    return true;
}

/**
 * Check if a timer operation is allowed and get the clamped interval.
 *
 * @param cap_val         Timer capability value.
 * @param requested_ms    Requested interval in milliseconds.
 * @param out_clamped_ms  Output: clamped interval (>= min_interval_ms).
 * @return                true if allowed, false if denied.
 */
bool r8e_capability_check_timer(R8EValue cap_val,
                                 uint32_t requested_ms,
                                 uint32_t *out_clamped_ms)
{
    if (!R8E_IS_POINTER(cap_val)) return false;

    R8ECapability *cap = (R8ECapability *)r8e_get_pointer(cap_val);
    if (!cap_is_usable(cap)) return false;
    if (cap->type != R8E_CAP_TIMER) return false;

    R8ECapTimer *timer = &cap->config.timer;

    /* Check permission */
    if (!(timer->permissions & R8E_PERM_TIMER_SET)) return false;

    /* Check concurrent timer limit */
    if (timer->max_concurrent > 0 &&
        timer->current_count >= timer->max_concurrent) {
        return false;
    }

    /* Clamp interval to minimum (Section 11.8: timer resolution clamping) */
    uint32_t clamped = requested_ms;
    if (clamped < timer->min_interval_ms) {
        clamped = timer->min_interval_ms;
    }

    if (out_clamped_ms) *out_clamped_ms = clamped;
    return true;
}

/**
 * Record timer creation (for concurrent count tracking).
 */
void r8e_capability_timer_created(R8EValue cap_val) {
    if (!R8E_IS_POINTER(cap_val)) return;
    R8ECapability *cap = (R8ECapability *)r8e_get_pointer(cap_val);
    if (!cap_validate(cap) || cap->type != R8E_CAP_TIMER) return;
    cap->config.timer.current_count++;
}

/**
 * Record timer destruction.
 */
void r8e_capability_timer_destroyed(R8EValue cap_val) {
    if (!R8E_IS_POINTER(cap_val)) return;
    R8ECapability *cap = (R8ECapability *)r8e_get_pointer(cap_val);
    if (!cap_validate(cap) || cap->type != R8E_CAP_TIMER) return;
    if (cap->config.timer.current_count > 0) {
        cap->config.timer.current_count--;
    }
}

/**
 * Check if an environment variable access is allowed.
 *
 * @param cap_val  Environment capability value.
 * @param name     Environment variable name.
 * @return         true if allowed, false if denied.
 */
bool r8e_capability_check_env(R8EValue cap_val, const char *name) {
    if (!R8E_IS_POINTER(cap_val) || !name) return false;

    R8ECapability *cap = (R8ECapability *)r8e_get_pointer(cap_val);
    if (!cap_is_usable(cap)) return false;
    if (cap->type != R8E_CAP_ENV) return false;

    R8ECapEnv *env = &cap->config.env;

    /* Check permission */
    if (!(env->permissions & R8E_PERM_ENV_READ)) return false;

    /* If no specific vars configured, allow all */
    if (env->var_count == 0) return true;

    /* Check against allowed variable patterns */
    for (uint8_t i = 0; i < env->var_count; i++) {
        const char *pattern = env->allowed_vars[i];
        /* Simple prefix match (e.g., "APP_" matches "APP_NAME") */
        size_t plen = strlen(pattern);
        if (plen > 0 && pattern[plen - 1] == '*') {
            /* Wildcard suffix: check prefix */
            if (memcmp(name, pattern, plen - 1) == 0) {
                return true;
            }
        } else {
            /* Exact match */
            if (strcmp(name, pattern) == 0) {
                return true;
            }
        }
    }

    return false;
}

/**
 * Check a generic capability permission.
 *
 * @param cap_val    Capability value.
 * @param operation  Permission flag to check.
 * @return           true if allowed.
 */
bool r8e_capability_check(R8EValue cap_val, uint32_t operation) {
    if (!R8E_IS_POINTER(cap_val)) return false;

    R8ECapability *cap = (R8ECapability *)r8e_get_pointer(cap_val);
    if (!cap_is_usable(cap)) return false;

    uint32_t perms = 0;
    switch (cap->type) {
    case R8E_CAP_FS:     perms = cap->config.fs.permissions; break;
    case R8E_CAP_NET:    perms = cap->config.net.permissions; break;
    case R8E_CAP_TIMER:  perms = cap->config.timer.permissions; break;
    case R8E_CAP_ENV:    perms = cap->config.env.permissions; break;
    case R8E_CAP_CRYPTO: perms = cap->config.crypto.permissions; break;
    default: return false;
    }

    return (perms & operation) != 0;
}

/* =========================================================================
 * Capability Revocation
 * ========================================================================= */

/**
 * Revoke a capability permanently.
 *
 * After revocation, ALL operations through this capability (and all
 * capabilities attenuated from it) will fail immediately.
 * Revocation is permanent and cannot be undone.
 *
 * @param cap_val  Capability value to revoke.
 */
void r8e_capability_revoke(R8EValue cap_val) {
    if (!R8E_IS_POINTER(cap_val)) return;

    R8ECapability *cap = (R8ECapability *)r8e_get_pointer(cap_val);
    if (!cap_validate(cap)) return;

    /* Mark as revoked. Since attenuated children check the parent chain,
     * this effectively revokes all descendants too. */
    cap->revoked = true;
}

/**
 * Check if a capability has been revoked.
 *
 * @param cap_val  Capability value.
 * @return         true if revoked (directly or via parent chain).
 */
bool r8e_capability_is_revoked(R8EValue cap_val) {
    if (!R8E_IS_POINTER(cap_val)) return true;

    R8ECapability *cap = (R8ECapability *)r8e_get_pointer(cap_val);
    return !cap_is_usable(cap);
}

/* =========================================================================
 * Capability Lifecycle
 * ========================================================================= */

/**
 * Increment the reference count of a capability.
 */
void r8e_capability_retain(R8EValue cap_val) {
    if (!R8E_IS_POINTER(cap_val)) return;
    R8ECapability *cap = (R8ECapability *)r8e_get_pointer(cap_val);
    if (!cap_validate(cap)) return;
    cap->refcount++;
}

/**
 * Decrement the reference count and free if zero.
 */
void r8e_capability_release(R8EValue cap_val) {
    if (!R8E_IS_POINTER(cap_val)) return;
    R8ECapability *cap = (R8ECapability *)r8e_get_pointer(cap_val);
    if (!cap_validate(cap)) return;

    if (cap->refcount > 0) cap->refcount--;
    if (cap->refcount == 0) {
        /* Release parent reference */
        if (cap->parent) {
            R8EValue parent_val = r8e_from_pointer(cap->parent);
            r8e_capability_release(parent_val);
        }

        /* Scrub and free */
        memset(cap, 0, sizeof(R8ECapability));
        free(cap);
    }
}

/**
 * Get the type of a capability.
 *
 * @param cap_val  Capability value.
 * @return         Capability type, or -1 if invalid.
 */
int r8e_capability_type(R8EValue cap_val) {
    if (!R8E_IS_POINTER(cap_val)) return -1;
    R8ECapability *cap = (R8ECapability *)r8e_get_pointer(cap_val);
    if (!cap_validate(cap)) return -1;
    return (int)cap->type;
}

/**
 * Get the owning realm ID of a capability.
 *
 * @param cap_val  Capability value.
 * @return         Realm ID, or UINT32_MAX if invalid.
 */
uint32_t r8e_capability_realm(R8EValue cap_val) {
    if (!R8E_IS_POINTER(cap_val)) return UINT32_MAX;
    R8ECapability *cap = (R8ECapability *)r8e_get_pointer(cap_val);
    if (!cap_validate(cap)) return UINT32_MAX;
    return cap->realm_id;
}

/**
 * Get a human-readable description of a capability (for debugging).
 *
 * @param cap_val  Capability value.
 * @param buf      Output buffer.
 * @param buflen   Buffer size.
 * @return         Number of characters written.
 */
int r8e_capability_describe(R8EValue cap_val, char *buf, size_t buflen) {
    if (!buf || buflen == 0) return 0;

    if (!R8E_IS_POINTER(cap_val)) {
        return snprintf(buf, buflen, "<invalid capability>");
    }

    R8ECapability *cap = (R8ECapability *)r8e_get_pointer(cap_val);
    if (!cap_validate(cap)) {
        return snprintf(buf, buflen, "<corrupted capability>");
    }

    const char *type_names[] = {
        "fs", "net", "timer", "env", "crypto", "custom"
    };
    const char *type_name = (cap->type < R8E_CAP_COUNT)
                            ? type_names[cap->type] : "unknown";

    int n = snprintf(buf, buflen,
                     "capability{type=%s, id=%u, realm=%u, revoked=%s",
                     type_name, cap->id, cap->realm_id,
                     cap->revoked ? "true" : "false");

    if (cap->type == R8E_CAP_FS && (size_t)n < buflen) {
        n += snprintf(buf + n, buflen - (size_t)n,
                      ", root=\"%s\", perms=0x%04X, quota=%zu",
                      cap->config.fs.root,
                      cap->config.fs.permissions,
                      cap->config.fs.quota_bytes);
    } else if (cap->type == R8E_CAP_NET && (size_t)n < buflen) {
        n += snprintf(buf + n, buflen - (size_t)n,
                      ", perms=0x%04X, hosts=%u",
                      cap->config.net.permissions,
                      cap->config.net.host_count);
    } else if (cap->type == R8E_CAP_TIMER && (size_t)n < buflen) {
        n += snprintf(buf + n, buflen - (size_t)n,
                      ", min_ms=%u, max_concurrent=%u",
                      cap->config.timer.min_interval_ms,
                      cap->config.timer.max_concurrent);
    }

    if ((size_t)n < buflen) {
        n += snprintf(buf + n, buflen - (size_t)n, "}");
    }

    return n;
}
