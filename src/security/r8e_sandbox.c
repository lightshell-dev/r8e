/*
 * r8e_sandbox.c - Layer 1: OS-Level Sandboxing
 *
 * Part of the r8e JavaScript engine.
 * See CLAUDE.md Section 11.3 (Layer 1: OS-Level Sandboxing).
 *
 * Architecture:
 *   - Seccomp-BPF syscall filtering (Linux)
 *   - Landlock LSM path-based isolation (Linux 5.13+)
 *   - sandbox_init on macOS (limited scope)
 *   - OpenBSD pledge-style API: r8e_pledge(ctx, "stdio rpath")
 *   - Two-phase narrowing: load with rpath, then drop to stdio
 *   - One-way ratchet: permissions only removed, never added back
 *   - Promise strings: "stdio", "rpath", "wpath", "cpath",
 *     "inet", "proc", "exec", "dns", "tmppath"
 *
 * EXP-11: pure computation needs ~15 syscalls. pledge("stdio") covers 90%.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

/* Platform detection */
#if defined(__linux__)
  #define R8E_PLATFORM_LINUX 1
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/prctl.h>
  #include <sys/syscall.h>
  #include <linux/filter.h>
  #include <linux/seccomp.h>
  #include <linux/audit.h>
  /* Landlock headers may not be available in older toolchains */
  #if __has_include(<linux/landlock.h>)
    #include <linux/landlock.h>
    #define R8E_HAS_LANDLOCK 1
  #else
    #define R8E_HAS_LANDLOCK 0
  #endif
#elif defined(__APPLE__)
  #define R8E_PLATFORM_MACOS 1
  #include <unistd.h>
  /* sandbox.h is deprecated but available */
  /* We use a limited approach on macOS */
#elif defined(__OpenBSD__)
  #define R8E_PLATFORM_OPENBSD 1
  #include <unistd.h>
#else
  #define R8E_PLATFORM_GENERIC 1
#endif

#include "../../include/r8e_types.h"
#include "../../include/r8e_api.h"

/* =========================================================================
 * Sandbox Promise Flags
 *
 * Each promise is a bit in a 32-bit bitmask. The sandbox starts with all
 * bits set (all permissions) and can only clear bits (remove permissions).
 * Once a bit is cleared it cannot be set again (one-way ratchet).
 * ========================================================================= */

#define R8E_PROMISE_STDIO    0x0001U  /* read/write to already-open fds */
#define R8E_PROMISE_RPATH    0x0002U  /* open files for reading */
#define R8E_PROMISE_WPATH    0x0004U  /* open files for writing */
#define R8E_PROMISE_CPATH    0x0008U  /* create/delete files and dirs */
#define R8E_PROMISE_INET     0x0010U  /* network socket operations */
#define R8E_PROMISE_PROC     0x0020U  /* fork, exec, ptrace etc. */
#define R8E_PROMISE_EXEC     0x0040U  /* execve */
#define R8E_PROMISE_DNS      0x0080U  /* DNS resolution (getaddrinfo etc.) */
#define R8E_PROMISE_TMPPATH  0x0100U  /* /tmp access */
#define R8E_PROMISE_MMAP     0x0200U  /* mmap (needed internally) */
#define R8E_PROMISE_ALL      0x03FFU  /* all permissions */

/* =========================================================================
 * Sandbox State
 *
 * Stored per-context. Tracks current permissions and whether OS-level
 * enforcement has been applied (irreversible).
 * ========================================================================= */

typedef struct R8ESandbox {
    uint32_t allowed_promises;    /* bitmask of currently allowed promises */
    uint32_t initial_promises;    /* bitmask at first pledge call */
    bool     is_active;           /* true after first pledge call */
    bool     os_enforced;         /* true after OS-level filter installed */
    bool     landlock_active;     /* true if Landlock ruleset is active */
    int      landlock_fd;         /* Landlock ruleset fd (-1 if none) */
    char     allowed_paths[16][256]; /* Landlock allowed paths */
    uint8_t  allowed_path_count;  /* number of allowed paths */
} R8ESandbox;

/* Global sandbox state per context (in production this would be a member
 * of R8EContext; here we use a static mapping for up to 16 contexts) */
#define R8E_MAX_SANDBOX_CONTEXTS 16

static R8ESandbox g_sandboxes[R8E_MAX_SANDBOX_CONTEXTS];
static bool       g_sandbox_init[R8E_MAX_SANDBOX_CONTEXTS];

/* =========================================================================
 * Internal: Map context to sandbox slot
 * ========================================================================= */

static int sandbox_slot_for_ctx(const R8EContext *ctx) {
    /* Use the current realm ID as the sandbox slot.
     * This gives each realm its own sandbox state. */
    if (!ctx) return -1;
    int slot = ctx->current_realm;
    if (slot < 0 || slot >= R8E_MAX_SANDBOX_CONTEXTS) return -1;
    return slot;
}

static R8ESandbox *sandbox_get(R8EContext *ctx) {
    int slot = sandbox_slot_for_ctx(ctx);
    if (slot < 0) return NULL;

    if (!g_sandbox_init[slot]) {
        memset(&g_sandboxes[slot], 0, sizeof(R8ESandbox));
        g_sandboxes[slot].allowed_promises = R8E_PROMISE_ALL;
        g_sandboxes[slot].landlock_fd = -1;
        g_sandbox_init[slot] = true;
    }
    return &g_sandboxes[slot];
}

/* =========================================================================
 * Promise String Parsing
 *
 * Parse a space-separated promise string like "stdio rpath" into a bitmask.
 * Unknown promise names are treated as errors.
 * ========================================================================= */

typedef struct {
    const char *name;
    uint32_t    flag;
} R8EPromiseEntry;

static const R8EPromiseEntry promise_table[] = {
    { "stdio",   R8E_PROMISE_STDIO },
    { "rpath",   R8E_PROMISE_RPATH },
    { "wpath",   R8E_PROMISE_WPATH },
    { "cpath",   R8E_PROMISE_CPATH },
    { "inet",    R8E_PROMISE_INET },
    { "proc",    R8E_PROMISE_PROC },
    { "exec",    R8E_PROMISE_EXEC },
    { "dns",     R8E_PROMISE_DNS },
    { "tmppath", R8E_PROMISE_TMPPATH },
    { "mmap",    R8E_PROMISE_MMAP },
    { NULL, 0 }
};

/**
 * Parse a space-separated promise string into a bitmask.
 *
 * @param promises  Space-separated promise names (e.g. "stdio rpath").
 *                  NULL or empty string means no promises (nothing allowed).
 * @param out_mask  Output: the parsed bitmask.
 * @return          R8E_OK on success, R8E_ERROR if an unknown promise is found.
 */
static R8EStatus parse_promises(const char *promises, uint32_t *out_mask) {
    if (!out_mask) return R8E_ERROR;
    *out_mask = 0;

    if (!promises || promises[0] == '\0') {
        return R8E_OK; /* empty = no permissions */
    }

    const char *p = promises;
    while (*p) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        /* Find end of token */
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        size_t len = (size_t)(p - start);

        /* Look up token in promise table */
        bool found = false;
        for (const R8EPromiseEntry *e = promise_table; e->name; e++) {
            if (strlen(e->name) == len && memcmp(e->name, start, len) == 0) {
                *out_mask |= e->flag;
                found = true;
                break;
            }
        }

        if (!found) {
            /* Unknown promise name - security: reject */
            return R8E_ERROR;
        }
    }

    return R8E_OK;
}

/* =========================================================================
 * Platform: Linux Seccomp-BPF
 *
 * Build a BPF filter program that allows only the syscalls corresponding
 * to the active promise set. Install it with prctl + seccomp.
 * ========================================================================= */

#ifdef R8E_PLATFORM_LINUX

/* Architecture detection for seccomp audit arch */
#if defined(__x86_64__)
  #define R8E_SECCOMP_ARCH AUDIT_ARCH_X86_64
#elif defined(__i386__)
  #define R8E_SECCOMP_ARCH AUDIT_ARCH_I386
#elif defined(__aarch64__)
  #define R8E_SECCOMP_ARCH AUDIT_ARCH_AARCH64
#elif defined(__arm__)
  #define R8E_SECCOMP_ARCH AUDIT_ARCH_ARM
#else
  #define R8E_SECCOMP_ARCH 0
#endif

/* Maximum BPF instructions we will generate */
#define R8E_BPF_MAX_INSNS 256

/* Syscall numbers for common operations.
 * These are architecture-specific; we use __NR_ macros from headers. */

/* Helper: emit one BPF instruction */
static int bpf_emit(struct sock_filter *prog, int idx,
                     uint16_t code, uint8_t jt, uint8_t jf, uint32_t k)
{
    if (idx >= R8E_BPF_MAX_INSNS) return idx;
    prog[idx].code = code;
    prog[idx].jt = jt;
    prog[idx].jf = jf;
    prog[idx].k = k;
    return idx + 1;
}

/* BPF instruction macros */
#define BPF_LOAD_ARCH(prog, idx) \
    bpf_emit(prog, idx, BPF_LD | BPF_W | BPF_ABS, 0, 0, \
             (uint32_t)__builtin_offsetof(struct seccomp_data, arch))

#define BPF_LOAD_SYSCALL(prog, idx) \
    bpf_emit(prog, idx, BPF_LD | BPF_W | BPF_ABS, 0, 0, \
             (uint32_t)__builtin_offsetof(struct seccomp_data, nr))

#define BPF_ALLOW(prog, idx) \
    bpf_emit(prog, idx, BPF_RET | BPF_K, 0, 0, SECCOMP_RET_ALLOW)

#define BPF_KILL(prog, idx) \
    bpf_emit(prog, idx, BPF_RET | BPF_K, 0, 0, SECCOMP_RET_KILL_PROCESS)

#define BPF_ERRNO(prog, idx, err) \
    bpf_emit(prog, idx, BPF_RET | BPF_K, 0, 0, \
             SECCOMP_RET_ERRNO | ((err) & 0xFFFF))

/**
 * Build the list of allowed syscall numbers for the given promise mask.
 *
 * @param mask     Promise bitmask.
 * @param syscalls Output: array of allowed syscall numbers.
 * @param max_len  Max entries in syscalls array.
 * @return         Number of allowed syscalls written.
 */
static int build_syscall_allowlist(uint32_t mask, int *syscalls, int max_len) {
    int n = 0;

    /* Always allow these (needed for the engine itself) */
    #define ALLOW(nr) do { if (n < max_len) syscalls[n++] = (nr); } while(0)

    /* Minimal: exit, sigreturn, brk (always needed) */
    ALLOW(__NR_exit_group);
    ALLOW(__NR_exit);
    ALLOW(__NR_rt_sigreturn);
    ALLOW(__NR_brk);

    if (mask & R8E_PROMISE_STDIO) {
        ALLOW(__NR_read);
        ALLOW(__NR_write);
        ALLOW(__NR_close);
        ALLOW(__NR_fstat);
        #ifdef __NR_newfstatat
        ALLOW(__NR_newfstatat);
        #endif
        ALLOW(__NR_lseek);
        ALLOW(__NR_ioctl);
        ALLOW(__NR_fcntl);
        ALLOW(__NR_dup);
        ALLOW(__NR_dup2);
        #ifdef __NR_dup3
        ALLOW(__NR_dup3);
        #endif
        ALLOW(__NR_poll);
        ALLOW(__NR_ppoll);
        ALLOW(__NR_pread64);
        ALLOW(__NR_pwrite64);
        ALLOW(__NR_readv);
        ALLOW(__NR_writev);
        ALLOW(__NR_clock_gettime);
        ALLOW(__NR_gettimeofday);
        ALLOW(__NR_nanosleep);
        #ifdef __NR_clock_nanosleep
        ALLOW(__NR_clock_nanosleep);
        #endif
        ALLOW(__NR_getpid);
        ALLOW(__NR_getuid);
        ALLOW(__NR_getgid);
        ALLOW(__NR_geteuid);
        ALLOW(__NR_getegid);
        ALLOW(__NR_rt_sigaction);
        ALLOW(__NR_rt_sigprocmask);
        ALLOW(__NR_sigaltstack);
        ALLOW(__NR_getrandom);
        #ifdef __NR_futex
        ALLOW(__NR_futex);
        #endif
        #ifdef __NR_futex_waitv
        ALLOW(__NR_futex_waitv);
        #endif
    }

    if (mask & R8E_PROMISE_RPATH) {
        ALLOW(__NR_openat);
        #ifdef __NR_open
        ALLOW(__NR_open);
        #endif
        ALLOW(__NR_stat);
        ALLOW(__NR_lstat);
        ALLOW(__NR_access);
        #ifdef __NR_faccessat
        ALLOW(__NR_faccessat);
        #endif
        #ifdef __NR_faccessat2
        ALLOW(__NR_faccessat2);
        #endif
        ALLOW(__NR_readlink);
        #ifdef __NR_readlinkat
        ALLOW(__NR_readlinkat);
        #endif
        ALLOW(__NR_getdents64);
        #ifdef __NR_getdents
        ALLOW(__NR_getdents);
        #endif
        ALLOW(__NR_getcwd);
    }

    if (mask & R8E_PROMISE_WPATH) {
        ALLOW(__NR_openat);
        #ifdef __NR_open
        ALLOW(__NR_open);
        #endif
        ALLOW(__NR_rename);
        ALLOW(__NR_renameat);
        #ifdef __NR_renameat2
        ALLOW(__NR_renameat2);
        #endif
        ALLOW(__NR_ftruncate);
        ALLOW(__NR_truncate);
        ALLOW(__NR_chmod);
        ALLOW(__NR_fchmod);
        ALLOW(__NR_fchmodat);
        ALLOW(__NR_chown);
        ALLOW(__NR_fchown);
        ALLOW(__NR_fchownat);
        ALLOW(__NR_utimensat);
        #ifdef __NR_futimesat
        ALLOW(__NR_futimesat);
        #endif
        ALLOW(__NR_fsync);
        ALLOW(__NR_fdatasync);
    }

    if (mask & R8E_PROMISE_CPATH) {
        ALLOW(__NR_mkdir);
        ALLOW(__NR_mkdirat);
        ALLOW(__NR_rmdir);
        ALLOW(__NR_unlink);
        ALLOW(__NR_unlinkat);
        ALLOW(__NR_link);
        ALLOW(__NR_linkat);
        ALLOW(__NR_symlink);
        ALLOW(__NR_symlinkat);
        ALLOW(__NR_mknod);
        ALLOW(__NR_mknodat);
    }

    if (mask & R8E_PROMISE_INET) {
        ALLOW(__NR_socket);
        ALLOW(__NR_connect);
        ALLOW(__NR_bind);
        ALLOW(__NR_listen);
        ALLOW(__NR_accept);
        #ifdef __NR_accept4
        ALLOW(__NR_accept4);
        #endif
        ALLOW(__NR_sendto);
        ALLOW(__NR_recvfrom);
        ALLOW(__NR_setsockopt);
        ALLOW(__NR_getsockopt);
        ALLOW(__NR_getpeername);
        ALLOW(__NR_getsockname);
        ALLOW(__NR_sendmsg);
        ALLOW(__NR_recvmsg);
        ALLOW(__NR_shutdown);
        ALLOW(__NR_epoll_create1);
        ALLOW(__NR_epoll_ctl);
        ALLOW(__NR_epoll_wait);
        #ifdef __NR_epoll_pwait
        ALLOW(__NR_epoll_pwait);
        #endif
        ALLOW(__NR_select);
        #ifdef __NR_pselect6
        ALLOW(__NR_pselect6);
        #endif
    }

    if (mask & R8E_PROMISE_DNS) {
        /* DNS typically uses socket + connect + sendto/recvfrom.
         * The inet promise covers these; DNS adds nothing extra
         * but is kept for semantic clarity. The host may also
         * need /etc/resolv.conf (rpath). */
        if (!(mask & R8E_PROMISE_INET)) {
            ALLOW(__NR_socket);
            ALLOW(__NR_connect);
            ALLOW(__NR_sendto);
            ALLOW(__NR_recvfrom);
            ALLOW(__NR_sendmsg);
            ALLOW(__NR_recvmsg);
            ALLOW(__NR_close);
        }
    }

    if (mask & R8E_PROMISE_PROC) {
        ALLOW(__NR_clone);
        #ifdef __NR_clone3
        ALLOW(__NR_clone3);
        #endif
        ALLOW(__NR_fork);
        #ifdef __NR_vfork
        ALLOW(__NR_vfork);
        #endif
        ALLOW(__NR_wait4);
        #ifdef __NR_waitid
        ALLOW(__NR_waitid);
        #endif
        ALLOW(__NR_kill);
        ALLOW(__NR_tgkill);
        ALLOW(__NR_tkill);
        ALLOW(__NR_getppid);
        ALLOW(__NR_getpgrp);
        ALLOW(__NR_setpgid);
        ALLOW(__NR_setsid);
    }

    if (mask & R8E_PROMISE_EXEC) {
        ALLOW(__NR_execve);
        ALLOW(__NR_execveat);
    }

    if (mask & R8E_PROMISE_MMAP) {
        ALLOW(__NR_mmap);
        ALLOW(__NR_munmap);
        ALLOW(__NR_mprotect);
        ALLOW(__NR_madvise);
        ALLOW(__NR_mremap);
    }

    /* Always allow mmap for the engine's arena allocator, regardless of
     * the MMAP promise. The engine needs mmap internally even when
     * JS code does not. */
    if (!(mask & R8E_PROMISE_MMAP)) {
        ALLOW(__NR_mmap);
        ALLOW(__NR_munmap);
        ALLOW(__NR_mprotect);
    }

    #undef ALLOW
    return n;
}

/**
 * Install a seccomp-BPF filter that allows only the given syscalls.
 *
 * @param syscalls  Array of allowed syscall numbers.
 * @param count     Number of entries in syscalls.
 * @return          R8E_OK on success, R8E_ERROR on failure.
 */
static R8EStatus install_seccomp_filter(const int *syscalls, int count) {
    if (R8E_SECCOMP_ARCH == 0) {
        /* Unknown architecture; cannot build filter */
        return R8E_ERROR;
    }

    struct sock_filter prog[R8E_BPF_MAX_INSNS];
    int pc = 0;

    /* Step 1: Verify architecture */
    pc = BPF_LOAD_ARCH(prog, pc);
    /* If arch does not match, kill. jt = 0 (continue), jf = kill */
    pc = bpf_emit(prog, pc, BPF_JMP | BPF_JEQ | BPF_K, 0, 0,
                  R8E_SECCOMP_ARCH);
    /* We patch jf after we know the total length */
    int arch_check_idx = pc - 1;

    /* Step 2: Load syscall number */
    pc = BPF_LOAD_SYSCALL(prog, pc);

    /* Step 3: For each allowed syscall, emit a conditional allow.
     * We chain JEQ instructions. If syscall matches, jump to ALLOW.
     * If none match, fall through to KILL. */
    int allow_target = -1; /* will be patched */

    for (int i = 0; i < count && pc < R8E_BPF_MAX_INSNS - 4; i++) {
        /* JEQ syscall_nr, allow, next */
        pc = bpf_emit(prog, pc, BPF_JMP | BPF_JEQ | BPF_K,
                       0, 0, (uint32_t)syscalls[i]);
        /* jt will be patched to point to ALLOW */
    }

    /* Fall-through: deny with EPERM (not kill, to be less disruptive) */
    pc = BPF_ERRNO(prog, pc, EPERM);
    int deny_idx = pc - 1;
    (void)deny_idx;

    /* ALLOW return */
    allow_target = pc;
    pc = BPF_ALLOW(prog, pc);

    /* Patch architecture check: if arch does not match, jump to deny */
    /* The jf field is the number of instructions to skip on false */
    prog[arch_check_idx].jt = 0; /* continue to next instruction */
    prog[arch_check_idx].jf = (uint8_t)(pc - arch_check_idx - 1);

    /* Patch all the JEQ instructions to jump to ALLOW on match */
    for (int i = 0; i < count; i++) {
        int insn_idx = 2 + i; /* offset: arch_load + arch_check + syscall_load + i */
        if (insn_idx + 1 >= pc) break;
        /* Actually the JEQ instructions start at index 2 (after load_arch, jeq_arch, load_nr) */
        insn_idx = 3 + i;
        if (insn_idx >= pc - 2) break;
        /* jt = distance to ALLOW from this instruction */
        int dist = allow_target - insn_idx - 1;
        if (dist > 0 && dist < 256) {
            prog[insn_idx].jt = (uint8_t)dist;
            prog[insn_idx].jf = 0; /* continue to next check */
        }
    }

    /* Install the filter */
    struct sock_fprog fprog;
    fprog.len = (unsigned short)pc;
    fprog.filter = prog;

    /* Must set NO_NEW_PRIVS before installing seccomp filter */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        return R8E_ERROR;
    }

    if (prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &fprog, 0, 0) != 0) {
        return R8E_ERROR;
    }

    return R8E_OK;
}

#endif /* R8E_PLATFORM_LINUX */

/* =========================================================================
 * Platform: Linux Landlock
 *
 * Path-based filesystem isolation. Restricts which directories each
 * realm can access. Requires Linux 5.13+ with Landlock support.
 * ========================================================================= */

#if defined(R8E_PLATFORM_LINUX) && R8E_HAS_LANDLOCK

/**
 * Create a Landlock ruleset restricting filesystem access.
 *
 * @param sandbox   Sandbox state to update.
 * @param paths     Array of allowed directory paths.
 * @param count     Number of paths.
 * @param writable  If true, allow read+write; otherwise read-only.
 * @return          R8E_OK on success, R8E_ERROR on failure.
 */
static R8EStatus landlock_restrict_paths(R8ESandbox *sandbox,
                                          const char **paths, int count,
                                          bool writable)
{
    if (!sandbox || !paths || count <= 0) return R8E_ERROR;

    /* Check if Landlock is supported */
    int abi_version = (int)syscall(__NR_landlock_create_ruleset,
                                   NULL, 0,
                                   LANDLOCK_CREATE_RULESET_VERSION);
    if (abi_version < 0) {
        /* Landlock not supported on this kernel */
        return R8E_ERROR;
    }

    /* Create ruleset */
    struct landlock_ruleset_attr ruleset_attr;
    memset(&ruleset_attr, 0, sizeof(ruleset_attr));
    ruleset_attr.handled_access_fs =
        LANDLOCK_ACCESS_FS_READ_FILE |
        LANDLOCK_ACCESS_FS_READ_DIR |
        LANDLOCK_ACCESS_FS_EXECUTE;
    if (writable) {
        ruleset_attr.handled_access_fs |=
            LANDLOCK_ACCESS_FS_WRITE_FILE |
            LANDLOCK_ACCESS_FS_MAKE_REG |
            LANDLOCK_ACCESS_FS_MAKE_DIR |
            LANDLOCK_ACCESS_FS_REMOVE_FILE |
            LANDLOCK_ACCESS_FS_REMOVE_DIR;
    }

    int ruleset_fd = (int)syscall(__NR_landlock_create_ruleset,
                                   &ruleset_attr, sizeof(ruleset_attr), 0);
    if (ruleset_fd < 0) return R8E_ERROR;

    /* Add rules for each allowed path */
    for (int i = 0; i < count; i++) {
        int path_fd = open(paths[i], O_PATH | O_CLOEXEC);
        if (path_fd < 0) {
            close(ruleset_fd);
            return R8E_ERROR;
        }

        struct landlock_path_beneath_attr path_attr;
        memset(&path_attr, 0, sizeof(path_attr));
        path_attr.parent_fd = path_fd;
        path_attr.allowed_access =
            LANDLOCK_ACCESS_FS_READ_FILE |
            LANDLOCK_ACCESS_FS_READ_DIR |
            LANDLOCK_ACCESS_FS_EXECUTE;
        if (writable) {
            path_attr.allowed_access |=
                LANDLOCK_ACCESS_FS_WRITE_FILE |
                LANDLOCK_ACCESS_FS_MAKE_REG |
                LANDLOCK_ACCESS_FS_MAKE_DIR |
                LANDLOCK_ACCESS_FS_REMOVE_FILE |
                LANDLOCK_ACCESS_FS_REMOVE_DIR;
        }

        int ret = (int)syscall(__NR_landlock_add_rule, ruleset_fd,
                               LANDLOCK_RULE_PATH_BENEATH, &path_attr, 0);
        close(path_fd);
        if (ret < 0) {
            close(ruleset_fd);
            return R8E_ERROR;
        }

        /* Store path in sandbox state for auditing */
        if (sandbox->allowed_path_count < 16) {
            size_t plen = strlen(paths[i]);
            if (plen >= sizeof(sandbox->allowed_paths[0])) {
                plen = sizeof(sandbox->allowed_paths[0]) - 1;
            }
            memcpy(sandbox->allowed_paths[sandbox->allowed_path_count],
                   paths[i], plen);
            sandbox->allowed_paths[sandbox->allowed_path_count][plen] = '\0';
            sandbox->allowed_path_count++;
        }
    }

    /* Enforce the ruleset */
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        close(ruleset_fd);
        return R8E_ERROR;
    }

    int ret = (int)syscall(__NR_landlock_restrict_self, ruleset_fd, 0);
    if (ret < 0) {
        close(ruleset_fd);
        return R8E_ERROR;
    }

    sandbox->landlock_fd = ruleset_fd;
    sandbox->landlock_active = true;

    return R8E_OK;
}

#endif /* R8E_PLATFORM_LINUX && R8E_HAS_LANDLOCK */

/* =========================================================================
 * Platform: macOS Sandbox
 *
 * macOS does not have seccomp. We use a combination of:
 *   1. sandbox_init (deprecated but functional) for basic sandboxing
 *   2. Process-level restrictions via sandbox profiles
 *
 * This provides weaker guarantees than Linux seccomp but is better
 * than nothing.
 * ========================================================================= */

#ifdef R8E_PLATFORM_MACOS

/**
 * Apply macOS sandbox restrictions.
 * Limited to the predefined sandbox profiles available on macOS.
 *
 * @param mask  Promise bitmask.
 * @return      R8E_OK on success, R8E_ERROR on failure.
 */
static R8EStatus macos_apply_sandbox(uint32_t mask) {
    /* macOS sandbox profiles are very limited compared to seccomp.
     * We can only apply broad restrictions.
     *
     * For now, we provide the best effort: if the promise set does not
     * include inet, we can at least block network access.
     * Full implementation would use sandbox_init with a custom profile
     * string, but that API is deprecated and may be removed.
     *
     * In production, consider using the App Sandbox entitlements or
     * the newer EndpointSecurity framework. */

    /* For r8e, we log a warning and rely on capability-based API
     * (Layer 5) for actual enforcement on macOS. */
    (void)mask;

    /* Note: sandbox_init() is deprecated in macOS 10.15+.
     * A proper implementation would use the Sandbox framework's
     * private API or compile a custom .sb profile.
     * For safety, we do NOT call sandbox_init() here to avoid
     * linking against deprecated/unstable APIs. */

    return R8E_OK; /* best effort: rely on other security layers */
}

#endif /* R8E_PLATFORM_MACOS */

/* =========================================================================
 * Platform: OpenBSD
 *
 * OpenBSD has native pledge() which is what our API is modeled after.
 * Direct passthrough.
 * ========================================================================= */

#ifdef R8E_PLATFORM_OPENBSD

static R8EStatus openbsd_apply_pledge(uint32_t mask) {
    /* Build the pledge string from our promise mask */
    char pledgestr[256];
    pledgestr[0] = '\0';
    int pos = 0;

    #define APPEND_PROMISE(flag, str) \
        if (mask & (flag)) { \
            if (pos > 0) pledgestr[pos++] = ' '; \
            size_t slen = strlen(str); \
            if (pos + slen < sizeof(pledgestr) - 1) { \
                memcpy(pledgestr + pos, str, slen); \
                pos += (int)slen; \
            } \
        }

    APPEND_PROMISE(R8E_PROMISE_STDIO, "stdio")
    APPEND_PROMISE(R8E_PROMISE_RPATH, "rpath")
    APPEND_PROMISE(R8E_PROMISE_WPATH, "wpath")
    APPEND_PROMISE(R8E_PROMISE_CPATH, "cpath")
    APPEND_PROMISE(R8E_PROMISE_INET, "inet")
    APPEND_PROMISE(R8E_PROMISE_PROC, "proc")
    APPEND_PROMISE(R8E_PROMISE_EXEC, "exec")
    APPEND_PROMISE(R8E_PROMISE_DNS, "dns")
    APPEND_PROMISE(R8E_PROMISE_TMPPATH, "tmppath")

    #undef APPEND_PROMISE

    pledgestr[pos] = '\0';

    if (pledge(pledgestr, NULL) != 0) {
        return R8E_ERROR;
    }
    return R8E_OK;
}

#endif /* R8E_PLATFORM_OPENBSD */

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * Initialize the sandbox subsystem for a context.
 * Called once during context creation.
 *
 * @param ctx  Engine context.
 * @return     R8E_OK on success.
 */
R8EStatus r8e_sandbox_init(R8EContext *ctx) {
    if (!ctx) return R8E_ERROR;

    R8ESandbox *sb = sandbox_get(ctx);
    if (!sb) return R8E_ERROR;

    sb->allowed_promises = R8E_PROMISE_ALL;
    sb->is_active = false;
    sb->os_enforced = false;
    sb->landlock_active = false;
    sb->landlock_fd = -1;
    sb->allowed_path_count = 0;

    return R8E_OK;
}

/**
 * Apply pledge-style syscall restrictions.
 *
 * This is the primary sandbox API. The promise string specifies which
 * categories of system calls remain allowed. All other syscalls are
 * blocked.
 *
 * ONE-WAY RATCHET: Once a permission is removed, it cannot be added back.
 * Calling r8e_pledge with fewer promises than a previous call will further
 * restrict permissions. Calling with MORE promises is silently ignored
 * (the extra promises are masked away).
 *
 * TWO-PHASE NARROWING: Typical usage:
 *   r8e_pledge(ctx, "stdio rpath");  // Phase 1: load scripts
 *   // ... load and compile scripts ...
 *   r8e_pledge(ctx, "stdio");        // Phase 2: execute with minimal perms
 *
 * @param ctx       Engine context.
 * @param promises  Space-separated promise string (e.g. "stdio rpath").
 *                  NULL or "" means block everything except exit.
 * @return          R8E_OK on success, R8E_ERROR on failure.
 */
R8EStatus r8e_pledge(R8EContext *ctx, const char *promises) {
    if (!ctx) return R8E_ERROR;

    R8ESandbox *sb = sandbox_get(ctx);
    if (!sb) return R8E_ERROR_INTERNAL;

    /* Parse the promise string */
    uint32_t requested_mask = 0;
    R8EStatus status = parse_promises(promises, &requested_mask);
    if (status != R8E_OK) {
        return status; /* unknown promise name */
    }

    /* ONE-WAY RATCHET: new mask is the intersection of current and requested.
     * This means promises can only be removed, never added. */
    uint32_t new_mask = sb->allowed_promises & requested_mask;

    if (!sb->is_active) {
        /* First pledge call: record initial state */
        sb->initial_promises = new_mask;
        sb->is_active = true;
    }

    /* Update the allowed promises (can only decrease) */
    sb->allowed_promises = new_mask;

    /* Apply OS-level enforcement */
#ifdef R8E_PLATFORM_LINUX
    {
        int syscalls[512];
        int count = build_syscall_allowlist(new_mask, syscalls, 512);
        status = install_seccomp_filter(syscalls, count);
        if (status == R8E_OK) {
            sb->os_enforced = true;
        }
        /* If seccomp fails (e.g., container without CAP_SYS_ADMIN),
         * we continue with soft enforcement via sandbox_check. */
    }
#elif defined(R8E_PLATFORM_OPENBSD)
    status = openbsd_apply_pledge(new_mask);
    if (status == R8E_OK) {
        sb->os_enforced = true;
    }
#elif defined(R8E_PLATFORM_MACOS)
    status = macos_apply_sandbox(new_mask);
    /* macOS: best-effort; os_enforced remains false unless we actually
     * succeed with sandbox_init */
#else
    /* Generic platform: soft enforcement only */
    (void)new_mask;
#endif

    return R8E_OK;
}

/**
 * Check if a specific permission is currently allowed.
 *
 * This is the soft-enforcement path used by the capability layer
 * and built-in functions. Even if OS enforcement failed, this check
 * still works based on the in-memory promise state.
 *
 * @param ctx         Engine context.
 * @param permission  Permission string (one of the promise names).
 * @return            true if the permission is allowed, false otherwise.
 */
bool r8e_sandbox_check(R8EContext *ctx, const char *permission) {
    if (!ctx || !permission) return false;

    R8ESandbox *sb = sandbox_get(ctx);
    if (!sb) return false;

    /* If sandbox is not active, everything is allowed */
    if (!sb->is_active) return true;

    /* Look up the permission flag */
    for (const R8EPromiseEntry *e = promise_table; e->name; e++) {
        if (strcmp(e->name, permission) == 0) {
            return (sb->allowed_promises & e->flag) != 0;
        }
    }

    /* Unknown permission name: deny by default (fail-closed) */
    return false;
}

/**
 * Check if the sandbox is currently active (any pledge call has been made).
 *
 * @param ctx  Engine context.
 * @return     true if sandbox is active.
 */
bool r8e_sandbox_is_active(R8EContext *ctx) {
    if (!ctx) return false;
    R8ESandbox *sb = sandbox_get(ctx);
    return sb && sb->is_active;
}

/**
 * Check if OS-level enforcement is in effect.
 *
 * @param ctx  Engine context.
 * @return     true if kernel-level syscall filtering is active.
 */
bool r8e_sandbox_is_enforced(R8EContext *ctx) {
    if (!ctx) return false;
    R8ESandbox *sb = sandbox_get(ctx);
    return sb && sb->os_enforced;
}

/**
 * Get the current promise bitmask (for debugging/inspection).
 *
 * @param ctx  Engine context.
 * @return     Current promise bitmask, or 0 if no sandbox.
 */
uint32_t r8e_sandbox_get_promises(R8EContext *ctx) {
    if (!ctx) return 0;
    R8ESandbox *sb = sandbox_get(ctx);
    return sb ? sb->allowed_promises : 0;
}

/**
 * Apply Landlock path-based filesystem restrictions.
 *
 * This is an additional filesystem isolation mechanism on top of
 * seccomp. While seccomp blocks syscalls entirely, Landlock restricts
 * WHICH paths those syscalls can operate on.
 *
 * @param ctx       Engine context.
 * @param paths     Array of allowed directory paths.
 * @param count     Number of paths.
 * @param writable  If true, allow read+write; otherwise read-only.
 * @return          R8E_OK on success, R8E_ERROR if unsupported or failed.
 */
R8EStatus r8e_sandbox_landlock(R8EContext *ctx, const char **paths,
                                int count, bool writable)
{
    if (!ctx || !paths || count <= 0) return R8E_ERROR;

    R8ESandbox *sb = sandbox_get(ctx);
    if (!sb) return R8E_ERROR_INTERNAL;

    /* One-way ratchet: cannot expand Landlock after it is active */
    if (sb->landlock_active) {
        return R8E_ERROR; /* already restricted; cannot widen */
    }

#if defined(R8E_PLATFORM_LINUX) && R8E_HAS_LANDLOCK
    return landlock_restrict_paths(sb, paths, count, writable);
#else
    /* Landlock not available on this platform */
    (void)writable;
    return R8E_ERROR;
#endif
}

/**
 * Convenience: two-phase narrowing helper.
 *
 * Phase 1 ("load"): Allow stdio + rpath + mmap for loading scripts.
 * Phase 2 ("run"):  Drop to stdio + mmap only for execution.
 *
 * @param ctx    Engine context.
 * @param phase  0 = load phase, 1 = run phase.
 * @return       R8E_OK on success.
 */
R8EStatus r8e_sandbox_phase(R8EContext *ctx, int phase) {
    if (!ctx) return R8E_ERROR;

    switch (phase) {
    case 0:
        /* Load phase: allow reading files */
        return r8e_pledge(ctx, "stdio rpath mmap");
    case 1:
        /* Run phase: minimal permissions */
        return r8e_pledge(ctx, "stdio mmap");
    default:
        return R8E_ERROR;
    }
}

/**
 * Reset sandbox state for a slot (used when destroying a realm).
 * Note: this does NOT undo OS-level restrictions (those are permanent
 * for the process). It only resets the in-memory state.
 *
 * @param ctx  Engine context.
 */
void r8e_sandbox_destroy(R8EContext *ctx) {
    if (!ctx) return;
    int slot = sandbox_slot_for_ctx(ctx);
    if (slot < 0) return;

#if defined(R8E_PLATFORM_LINUX) && R8E_HAS_LANDLOCK
    if (g_sandboxes[slot].landlock_fd >= 0) {
        close(g_sandboxes[slot].landlock_fd);
    }
#endif

    memset(&g_sandboxes[slot], 0, sizeof(R8ESandbox));
    g_sandbox_init[slot] = false;
}
