/*
 * r8e_regexp.c - Tiered Regular Expression Engine
 *
 * Part of the r8e JavaScript engine. See CLAUDE.md Section 10.
 *
 * Two-tier engine:
 *   Tier 1: Backtracking (simple patterns) with fuel counter (1M max)
 *   Tier 2: Bitset NFA64 (complex patterns, <64 states in uint64_t)
 *
 * Supported: . ^ $ * + ? {n,m} [...] [^...] (...) (?:...) (?=...) (?!...)
 *   \d \D \w \W \s \S \b \B | \1-\9   Flags: g i m s u y
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* ---- Self-contained NaN-boxing type defs ---- */

typedef uint64_t R8EValue;

#define R8E_UNDEFINED  0xFFFA000000000000ULL
#define R8E_NULL       0xFFFA000000000001ULL
#define R8E_TRUE       0xFFFA000000000002ULL
#define R8E_FALSE      0xFFFA000000000003ULL

#define R8E_IS_DOUBLE(v)     ((v) < 0xFFF8000000000000ULL)
#define R8E_IS_INT32(v)      (((v) >> 48) == 0xFFF8U)
#define R8E_IS_POINTER(v)    (((v) >> 48) == 0xFFF9U)
#define R8E_IS_INLINE_STR(v) (((v) >> 48) == 0xFFFDU)

static inline int32_t r8e_get_int32(uint64_t v) { return (int32_t)(v & 0xFFFFFFFF); }
static inline void *r8e_get_pointer(uint64_t v) { return (void *)(uintptr_t)(v & 0x0000FFFFFFFFFFFFULL); }
static inline uint64_t r8e_from_int32(int32_t i) { return 0xFFF8000000000000ULL | (uint32_t)i; }
static inline uint64_t r8e_from_pointer(void *p) { return 0xFFF9000000000000ULL | (uint64_t)(uintptr_t)p; }
static inline uint64_t r8e_from_double(double d) { uint64_t v; memcpy(&v, &d, 8); return v; }
static inline R8EValue r8e_from_bool(bool b) { return b ? R8E_TRUE : R8E_FALSE; }

typedef struct R8EContext R8EContext;
struct R8EContext { void *arena; };

/* Atom IDs used by regexp */
#define R8E_ATOM_exec       172
#define R8E_ATOM_test       171
#define R8E_ATOM_source     173
#define R8E_ATOM_flags      174
#define R8E_ATOM_global     175
#define R8E_ATOM_ignoreCase 176
#define R8E_ATOM_multiline  177
#define R8E_ATOM_dotAll     178
#define R8E_ATOM_unicode    179
#define R8E_ATOM_sticky     180
#define R8E_ATOM_index      285
#define R8E_ATOM_input      286
#define R8E_ATOM_groups     287
#define R8E_ATOM_match      160
#define R8E_ATOM_matchAll   161
#define R8E_ATOM_search     162
#define R8E_ATOM_replace    157
#define R8E_ATOM_split      159
#define R8E_ATOM_toString   4
#define R8E_ATOM_length     1
#define R8E_ATOM_lastIndex  285

/* GC constants */
#define R8E_GC_KIND_REGEXP     6
#define R8E_GC_KIND_SHIFT      5
#define R8E_GC_RC_INLINE_SHIFT 16
#define R8E_GC_MARK            0x00000004u
#define R8E_PROTO_REGEXP       7

/* ---- Regex flags ---- */

#define R8E_RE_GLOBAL      0x01u
#define R8E_RE_IGNORECASE  0x02u
#define R8E_RE_MULTILINE   0x04u
#define R8E_RE_DOTALL      0x08u
#define R8E_RE_UNICODE     0x10u
#define R8E_RE_STICKY      0x20u

#define R8E_REGEX_FUEL_MAX 1000000
#define R8E_RE_MAX_GROUPS  32
#define R8E_RE_MAX_RANGES  64
#define R8E_RE_NODE_POOL   512

/* ---- Regex AST node types ---- */

typedef enum {
    RE_LITERAL, RE_DOT, RE_CHAR_CLASS, RE_ANCHOR_START, RE_ANCHOR_END,
    RE_WORD_BOUND, RE_NOT_WORD_BOUND, RE_QUANTIFIER, RE_GROUP, RE_NCGROUP,
    RE_LOOKAHEAD, RE_NEG_LOOKAHEAD, RE_ALT, RE_CONCAT, RE_BACKREF,
    RE_SHORTHAND, RE_EMPTY
} R8EReNodeType;

typedef struct { uint32_t lo, hi; } R8ECharRange;

typedef struct R8EReNode R8EReNode;
struct R8EReNode {
    R8EReNodeType type;
    union {
        uint32_t ch;
        struct { R8ECharRange ranges[R8E_RE_MAX_RANGES]; uint16_t count; bool negated; } cc;
        struct { R8EReNode *child; uint32_t min, max; bool greedy; } quant;
        struct { R8EReNode *child; uint32_t gid; } group;
        struct { R8EReNode *left, *right; } alt;
        struct { R8EReNode **children; uint32_t count, cap; } cat;
        uint32_t backref_id;
        char shorthand;
    } u;
};

/* ---- NFA64 ---- */

typedef struct {
    uint64_t char_masks[256];
    uint64_t epsilon[64];
    uint64_t accept_mask;
    uint8_t  num_states;
    uint8_t  group_starts[32];
    uint8_t  group_ends[32];
    uint8_t  group_count;
} R8ENFA64;

/* ---- R8ERegExp ---- */

typedef struct R8ERegExp {
    uint32_t flags;
    uint32_t proto_id;
    uint32_t re_flags;
    uint32_t last_index;
    char    *pattern;
    uint32_t pattern_len;
    uint8_t  engine; /* 0=BT, 1=NFA */
    union {
        struct {
            R8EReNode *root;
            R8EReNode *block;   /* base of allocated node block */
            uint32_t node_count;
            uint32_t group_count;
        } bt;
        R8ENFA64 nfa;
    } compiled;
} R8ERegExp;

/* ---- Match result ---- */

typedef struct { int32_t start, end; } R8EMatchGroup;
typedef struct {
    bool matched;
    int32_t match_start, match_end;
    R8EMatchGroup groups[R8E_RE_MAX_GROUPS];
    uint32_t group_count;
} R8EMatchResult;

/* ---- Node pool ---- */

typedef struct { R8EReNode nodes[R8E_RE_NODE_POOL]; uint32_t used; } R8EReNodePool;

static R8EReNode *pool_alloc(R8EReNodePool *p) {
    if (p->used >= R8E_RE_NODE_POOL) return NULL;
    R8EReNode *n = &p->nodes[p->used++];
    memset(n, 0, sizeof(*n));
    return n;
}

/* ---- Character helpers ---- */

static inline bool is_digit(uint32_t c) { return c >= '0' && c <= '9'; }
static inline bool is_word(uint32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}
static inline bool is_space(uint32_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\f' || c == '\v' || c == 0x00A0 || c == 0xFEFF ||
           c == 0x2028 || c == 0x2029;
}
static inline bool match_sh(char sh, uint32_t c) {
    switch (sh) {
    case 'd': return is_digit(c);  case 'D': return !is_digit(c);
    case 'w': return is_word(c);   case 'W': return !is_word(c);
    case 's': return is_space(c);  case 'S': return !is_space(c);
    default:  return false;
    }
}
static inline bool char_in_cc(const R8EReNode *n, uint32_t c) {
    bool found = false;
    for (uint16_t i = 0; i < n->u.cc.count; i++)
        if (c >= n->u.cc.ranges[i].lo && c <= n->u.cc.ranges[i].hi) { found = true; break; }
    return n->u.cc.negated ? !found : found;
}
static inline uint32_t to_lower(uint32_t c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* ---- UTF-8 decode ---- */

static uint32_t utf8_decode(const char *s, int len, int pos, int *br) {
    if (pos >= len) { *br = 0; return 0; }
    uint8_t b = (uint8_t)s[pos];
    if (b < 0x80) { *br = 1; return b; }
    if ((b & 0xE0) == 0xC0 && pos + 1 < len) {
        *br = 2; return ((uint32_t)(b & 0x1F) << 6) | ((uint8_t)s[pos+1] & 0x3F);
    }
    if ((b & 0xF0) == 0xE0 && pos + 2 < len) {
        *br = 3;
        return ((uint32_t)(b & 0x0F) << 12) |
               (((uint32_t)((uint8_t)s[pos+1]) & 0x3F) << 6) |
               ((uint8_t)s[pos+2] & 0x3F);
    }
    if ((b & 0xF8) == 0xF0 && pos + 3 < len) {
        *br = 4;
        return ((uint32_t)(b & 0x07) << 18) |
               (((uint32_t)((uint8_t)s[pos+1]) & 0x3F) << 12) |
               (((uint32_t)((uint8_t)s[pos+2]) & 0x3F) << 6) |
               ((uint8_t)s[pos+3] & 0x3F);
    }
    *br = 1; return 0xFFFD;
}

/* ==== REGEX PARSER ==== */

typedef struct {
    const char *src; uint32_t len, pos, re_flags;
    R8EReNodePool *pool; uint32_t groups, depth;
    int err; char errmsg[128];
} ReParser;

static R8EReNode *re_parse_alt(ReParser *p);

static void re_err(ReParser *p, const char *m) {
    p->err = -1;
    size_t n = strlen(m); if (n > 127) n = 127;
    memcpy(p->errmsg, m, n); p->errmsg[n] = '\0';
}

static inline uint32_t re_peek(ReParser *p) { return (p->pos < p->len) ? (uint8_t)p->src[p->pos] : 0; }
static inline uint32_t re_adv(ReParser *p) { return (p->pos < p->len) ? (uint8_t)p->src[p->pos++] : 0; }
static inline bool re_match(ReParser *p, uint32_t c) {
    if (p->pos < p->len && (uint8_t)p->src[p->pos] == c) { p->pos++; return true; }
    return false;
}

static int32_t re_parse_int(ReParser *p) {
    int32_t v = 0; bool has = false;
    while (p->pos < p->len && (uint8_t)p->src[p->pos] >= '0' && (uint8_t)p->src[p->pos] <= '9') {
        v = v * 10 + ((uint8_t)p->src[p->pos] - '0');
        if (v > 100000) { re_err(p, "quantifier too large"); return -1; }
        p->pos++; has = true;
    }
    return has ? v : -1;
}

/* Parse [...] */
static R8EReNode *re_parse_cc(ReParser *p) {
    R8EReNode *n = pool_alloc(p->pool);
    if (!n) { re_err(p, "out of nodes"); return NULL; }
    n->type = RE_CHAR_CLASS; n->u.cc.count = 0; n->u.cc.negated = false;
    if (re_match(p, '^')) n->u.cc.negated = true;

    bool first = true;
    while (p->pos < p->len && (first || re_peek(p) != ']')) {
        first = false;
        if (p->err) return NULL;
        if (n->u.cc.count >= R8E_RE_MAX_RANGES) { re_err(p, "too many ranges"); return NULL; }

        uint32_t lo;
        if (re_peek(p) == '\\' && p->pos + 1 < p->len) {
            p->pos++;
            uint32_t e = re_adv(p);
            switch (e) {
            case 'd': case 'D': case 'w': case 'W': case 's': case 'S': {
                if (e == 'd' || e == 'D') {
                    n->u.cc.ranges[n->u.cc.count++] = (R8ECharRange){'0', '9'};
                    if (e == 'D') n->u.cc.negated = !n->u.cc.negated;
                } else if (e == 'w' || e == 'W') {
                    if (n->u.cc.count + 4 > R8E_RE_MAX_RANGES) { re_err(p, "too many ranges"); return NULL; }
                    n->u.cc.ranges[n->u.cc.count++] = (R8ECharRange){'a','z'};
                    n->u.cc.ranges[n->u.cc.count++] = (R8ECharRange){'A','Z'};
                    n->u.cc.ranges[n->u.cc.count++] = (R8ECharRange){'0','9'};
                    n->u.cc.ranges[n->u.cc.count++] = (R8ECharRange){'_','_'};
                    if (e == 'W') n->u.cc.negated = !n->u.cc.negated;
                } else {
                    if (n->u.cc.count + 5 > R8E_RE_MAX_RANGES) { re_err(p, "too many ranges"); return NULL; }
                    n->u.cc.ranges[n->u.cc.count++] = (R8ECharRange){' ',' '};
                    n->u.cc.ranges[n->u.cc.count++] = (R8ECharRange){'\t','\r'};
                    n->u.cc.ranges[n->u.cc.count++] = (R8ECharRange){0x00A0,0x00A0};
                    n->u.cc.ranges[n->u.cc.count++] = (R8ECharRange){0xFEFF,0xFEFF};
                    n->u.cc.ranges[n->u.cc.count++] = (R8ECharRange){0x2028,0x2029};
                    if (e == 'S') n->u.cc.negated = !n->u.cc.negated;
                }
                continue;
            }
            case 'n': lo = '\n'; break; case 'r': lo = '\r'; break;
            case 't': lo = '\t'; break; case 'f': lo = '\f'; break;
            case 'v': lo = '\v'; break; case '0': lo = '\0'; break;
            default: lo = e; break;
            }
        } else {
            lo = re_adv(p);
        }
        uint32_t hi = lo;
        if (re_peek(p) == '-' && p->pos + 1 < p->len && (uint8_t)p->src[p->pos+1] != ']') {
            p->pos++;
            if (re_peek(p) == '\\' && p->pos + 1 < p->len) {
                p->pos++;
                uint32_t e = re_adv(p);
                switch (e) { case 'n': hi='\n'; break; case 'r': hi='\r'; break;
                             case 't': hi='\t'; break; default: hi=e; break; }
            } else hi = re_adv(p);
            if (hi < lo) { re_err(p, "invalid range"); return NULL; }
        }
        n->u.cc.ranges[n->u.cc.count].lo = lo;
        n->u.cc.ranges[n->u.cc.count].hi = hi;
        n->u.cc.count++;
    }
    if (!re_match(p, ']')) { re_err(p, "unterminated char class"); return NULL; }
    return n;
}

/* Parse escape */
static R8EReNode *re_parse_esc(ReParser *p) {
    if (p->pos >= p->len) { re_err(p, "trailing backslash"); return NULL; }
    uint32_t c = re_adv(p);
    switch (c) {
    case 'd': case 'D': case 'w': case 'W': case 's': case 'S': {
        R8EReNode *n = pool_alloc(p->pool);
        if (!n) return NULL;
        n->type = RE_SHORTHAND; n->u.shorthand = (char)c; return n;
    }
    case 'b': { R8EReNode *n = pool_alloc(p->pool); if (!n) return NULL;
                n->type = RE_WORD_BOUND; return n; }
    case 'B': { R8EReNode *n = pool_alloc(p->pool); if (!n) return NULL;
                n->type = RE_NOT_WORD_BOUND; return n; }
    case 'n': c='\n'; break; case 'r': c='\r'; break; case 't': c='\t'; break;
    case 'f': c='\f'; break; case 'v': c='\v'; break; case '0': c='\0'; break;
    default:
        if (c >= '1' && c <= '9') {
            R8EReNode *n = pool_alloc(p->pool); if (!n) return NULL;
            n->type = RE_BACKREF; n->u.backref_id = c - '0'; return n;
        }
        break;
    }
    R8EReNode *n = pool_alloc(p->pool);
    if (!n) return NULL;
    n->type = RE_LITERAL; n->u.ch = c; return n;
}

/* Parse atom */
static R8EReNode *re_parse_atom(ReParser *p) {
    if (p->err || p->pos >= p->len) return NULL;
    uint32_t c = re_peek(p);
    switch (c) {
    case '(': {
        p->pos++;
        R8EReNode *node;
        if (re_peek(p) == '?' && p->pos + 1 < p->len) {
            uint32_t nx = (uint8_t)p->src[p->pos+1];
            R8EReNodeType tp = RE_NCGROUP;
            if (nx == ':') { p->pos += 2; tp = RE_NCGROUP; }
            else if (nx == '=') { p->pos += 2; tp = RE_LOOKAHEAD; }
            else if (nx == '!') { p->pos += 2; tp = RE_NEG_LOOKAHEAD; }
            else goto capture;
            p->depth++;
            R8EReNode *child = re_parse_alt(p);
            p->depth--;
            if (p->err) return NULL;
            if (!re_match(p, ')')) { re_err(p, "unterminated group"); return NULL; }
            node = pool_alloc(p->pool); if (!node) return NULL;
            node->type = tp; node->u.group.child = child; node->u.group.gid = 0;
            return node;
        }
capture:;
        uint32_t gid = ++p->groups;
        if (gid >= R8E_RE_MAX_GROUPS) { re_err(p, "too many groups"); return NULL; }
        p->depth++;
        R8EReNode *child = re_parse_alt(p);
        p->depth--;
        if (p->err) return NULL;
        if (!re_match(p, ')')) { re_err(p, "unterminated group"); return NULL; }
        node = pool_alloc(p->pool); if (!node) return NULL;
        node->type = RE_GROUP; node->u.group.child = child; node->u.group.gid = gid;
        return node;
    }
    case '.': { p->pos++; R8EReNode *n = pool_alloc(p->pool); if (!n) return NULL;
                n->type = RE_DOT; return n; }
    case '^': { p->pos++; R8EReNode *n = pool_alloc(p->pool); if (!n) return NULL;
                n->type = RE_ANCHOR_START; return n; }
    case '$': { p->pos++; R8EReNode *n = pool_alloc(p->pool); if (!n) return NULL;
                n->type = RE_ANCHOR_END; return n; }
    case '[': { p->pos++; return re_parse_cc(p); }
    case '\\': { p->pos++; return re_parse_esc(p); }
    case ')': case '|': return NULL;
    case '*': case '+': case '?': case '{': re_err(p, "nothing to repeat"); return NULL;
    default: {
        p->pos++;
        R8EReNode *n = pool_alloc(p->pool); if (!n) return NULL;
        n->type = RE_LITERAL; n->u.ch = c; return n;
    }
    }
}

/* Parse atom + optional quantifier */
static R8EReNode *re_parse_quant(ReParser *p) {
    R8EReNode *atom = re_parse_atom(p);
    if (!atom || p->err || p->pos >= p->len) return atom;

    uint32_t qmin = 0, qmax = 0; bool has = false;
    uint32_t c = re_peek(p);
    switch (c) {
    case '*': p->pos++; qmin=0; qmax=UINT32_MAX; has=true; break;
    case '+': p->pos++; qmin=1; qmax=UINT32_MAX; has=true; break;
    case '?': p->pos++; qmin=0; qmax=1; has=true; break;
    case '{': {
        uint32_t sv = p->pos; p->pos++;
        int32_t lo = re_parse_int(p);
        if (lo < 0) { p->pos = sv; return atom; }
        qmin = (uint32_t)lo; qmax = qmin;
        if (re_match(p, ',')) { int32_t hi = re_parse_int(p); qmax = (hi >= 0) ? (uint32_t)hi : UINT32_MAX; }
        if (!re_match(p, '}')) { p->pos = sv; return atom; }
        if (qmax < qmin) { re_err(p, "invalid quantifier"); return NULL; }
        has = true; break;
    }
    }
    if (!has) return atom;

    bool greedy = true;
    if (p->pos < p->len && re_peek(p) == '?') { p->pos++; greedy = false; }

    R8EReNode *q = pool_alloc(p->pool); if (!q) return NULL;
    q->type = RE_QUANTIFIER;
    q->u.quant.child = atom; q->u.quant.min = qmin; q->u.quant.max = qmax; q->u.quant.greedy = greedy;
    return q;
}

/* Parse concatenation */
static R8EReNode *re_parse_seq(ReParser *p) {
    R8EReNode *local[32]; R8EReNode **ch = local;
    uint32_t cnt = 0, cap = 32;

    while (p->pos < p->len && !p->err) {
        uint32_t c = re_peek(p);
        if (c == ')' || c == '|') break;
        R8EReNode *child = re_parse_quant(p);
        if (!child) break;
        if (cnt >= cap) {
            uint32_t nc = cap * 2;
            R8EReNode **nw = (R8EReNode **)malloc(nc * sizeof(R8EReNode *));
            if (!nw) { re_err(p, "OOM"); return NULL; }
            memcpy(nw, ch, cnt * sizeof(R8EReNode *));
            if (ch != local) free(ch);
            ch = nw; cap = nc;
        }
        ch[cnt++] = child;
    }
    if (cnt == 0) {
        R8EReNode *e = pool_alloc(p->pool);
        if (e) e->type = RE_EMPTY;
        if (ch != local) free(ch);
        return e;
    }
    if (cnt == 1) { R8EReNode *r = ch[0]; if (ch != local) free(ch); return r; }

    R8EReNode *n = pool_alloc(p->pool);
    if (!n) { if (ch != local) free(ch); return NULL; }
    n->type = RE_CONCAT;
    R8EReNode **heap = (R8EReNode **)malloc(cnt * sizeof(R8EReNode *));
    if (!heap) { if (ch != local) free(ch); re_err(p, "OOM"); return NULL; }
    memcpy(heap, ch, cnt * sizeof(R8EReNode *));
    if (ch != local) free(ch);
    n->u.cat.children = heap; n->u.cat.count = cnt; n->u.cat.cap = cnt;
    return n;
}

/* Parse alternation */
static R8EReNode *re_parse_alt(ReParser *p) {
    R8EReNode *left = re_parse_seq(p);
    if (p->err) return NULL;
    while (p->pos < p->len && re_peek(p) == '|') {
        p->pos++;
        R8EReNode *right = re_parse_seq(p);
        if (p->err) return NULL;
        R8EReNode *a = pool_alloc(p->pool); if (!a) return NULL;
        a->type = RE_ALT; a->u.alt.left = left; a->u.alt.right = right;
        left = a;
    }
    return left;
}

/* ==== COMPLEXITY ANALYSIS ==== */

typedef struct {
    uint32_t groups, alts, max_qd, cur_qd;
    bool backrefs, lookahead;
} ReCx;

static void re_cx(R8EReNode *n, ReCx *c) {
    if (!n) return;
    switch (n->type) {
    case RE_GROUP: c->groups++; re_cx(n->u.group.child, c); break;
    case RE_NCGROUP: re_cx(n->u.group.child, c); break;
    case RE_LOOKAHEAD: case RE_NEG_LOOKAHEAD:
        c->lookahead = true; re_cx(n->u.group.child, c); break;
    case RE_ALT: c->alts++; re_cx(n->u.alt.left, c); re_cx(n->u.alt.right, c); break;
    case RE_CONCAT:
        for (uint32_t i = 0; i < n->u.cat.count; i++) re_cx(n->u.cat.children[i], c);
        break;
    case RE_QUANTIFIER:
        c->cur_qd++;
        if (c->cur_qd > c->max_qd) c->max_qd = c->cur_qd;
        re_cx(n->u.quant.child, c);
        c->cur_qd--;
        break;
    case RE_BACKREF: c->backrefs = true; break;
    default: break;
    }
}

static bool re_is_simple(R8EReNode *root) {
    ReCx c; memset(&c, 0, sizeof(c));
    re_cx(root, &c);
    if (c.backrefs) return true; /* NFA cannot handle backrefs */
    if (c.max_qd >= 2) return false;
    if (c.groups >= 3 && c.alts >= 3) return false;
    return true;
}

/* ==== TIER 1: BACKTRACKING ==== */

typedef struct {
    const char *input; int32_t len;
    R8EMatchGroup groups[R8E_RE_MAX_GROUPS]; uint32_t gcnt;
    int32_t fuel; uint32_t re_flags;
} BTCtx;

static int32_t bt_match(BTCtx *ctx, R8EReNode *node, int32_t pos);

static inline bool bt_wb(BTCtx *ctx, int32_t pos) {
    bool pw = (pos > 0) && is_word((uint8_t)ctx->input[pos-1]);
    bool cw = (pos < ctx->len) && is_word((uint8_t)ctx->input[pos]);
    return pw != cw;
}

static int32_t bt_node(BTCtx *ctx, R8EReNode *n, int32_t pos) {
    if (!n) return pos;
    switch (n->type) {
    case RE_LITERAL:
        if (pos >= ctx->len) return -1;
        if (ctx->re_flags & R8E_RE_IGNORECASE)
            return (to_lower((uint8_t)ctx->input[pos]) == to_lower(n->u.ch)) ? pos+1 : -1;
        return ((uint8_t)ctx->input[pos] == n->u.ch) ? pos+1 : -1;
    case RE_DOT:
        if (pos >= ctx->len) return -1;
        if (!(ctx->re_flags & R8E_RE_DOTALL) &&
            ((uint8_t)ctx->input[pos] == '\n' || (uint8_t)ctx->input[pos] == '\r')) return -1;
        return pos+1;
    case RE_SHORTHAND:
        if (pos >= ctx->len) return -1;
        return match_sh(n->u.shorthand, (uint8_t)ctx->input[pos]) ? pos+1 : -1;
    case RE_CHAR_CLASS:
        if (pos >= ctx->len) return -1;
        return char_in_cc(n, (uint8_t)ctx->input[pos]) ? pos+1 : -1;
    case RE_ANCHOR_START:
        if (ctx->re_flags & R8E_RE_MULTILINE)
            return (pos == 0 || ctx->input[pos-1] == '\n') ? pos : -1;
        return pos == 0 ? pos : -1;
    case RE_ANCHOR_END:
        if (ctx->re_flags & R8E_RE_MULTILINE)
            return (pos == ctx->len || ctx->input[pos] == '\n') ? pos : -1;
        return pos == ctx->len ? pos : -1;
    case RE_WORD_BOUND: return bt_wb(ctx, pos) ? pos : -1;
    case RE_NOT_WORD_BOUND: return bt_wb(ctx, pos) ? -1 : pos;
    case RE_EMPTY: return pos;
    case RE_BACKREF: {
        uint32_t g = n->u.backref_id;
        if (g >= ctx->gcnt || ctx->groups[g].start < 0) return pos;
        int32_t gs = ctx->groups[g].start, gl = ctx->groups[g].end - gs;
        if (pos + gl > ctx->len) return -1;
        for (int32_t i = 0; i < gl; i++) {
            uint32_t a = (uint8_t)ctx->input[pos+i], b = (uint8_t)ctx->input[gs+i];
            if (ctx->re_flags & R8E_RE_IGNORECASE) { if (to_lower(a) != to_lower(b)) return -1; }
            else { if (a != b) return -1; }
        }
        return pos + gl;
    }
    default: return -1;
    }
}

static int32_t bt_match(BTCtx *ctx, R8EReNode *n, int32_t pos) {
    if (!n) return pos;
    if (--ctx->fuel <= 0) return -1;

    switch (n->type) {
    case RE_LITERAL: case RE_DOT: case RE_SHORTHAND: case RE_CHAR_CLASS:
    case RE_ANCHOR_START: case RE_ANCHOR_END: case RE_WORD_BOUND:
    case RE_NOT_WORD_BOUND: case RE_EMPTY: case RE_BACKREF:
        return bt_node(ctx, n, pos);

    case RE_CONCAT: {
        int32_t cur = pos;
        for (uint32_t i = 0; i < n->u.cat.count; i++) {
            cur = bt_match(ctx, n->u.cat.children[i], cur);
            if (cur < 0) return -1;
        }
        return cur;
    }
    case RE_ALT: {
        int32_t r = bt_match(ctx, n->u.alt.left, pos);
        return (r >= 0) ? r : bt_match(ctx, n->u.alt.right, pos);
    }
    case RE_GROUP: {
        uint32_t g = n->u.group.gid;
        R8EMatchGroup saved = {-1, -1};
        if (g < R8E_RE_MAX_GROUPS) { saved = ctx->groups[g]; ctx->groups[g].start = pos; }
        int32_t r = bt_match(ctx, n->u.group.child, pos);
        if (r >= 0) { if (g < R8E_RE_MAX_GROUPS) ctx->groups[g].end = r; return r; }
        if (g < R8E_RE_MAX_GROUPS) ctx->groups[g] = saved;
        return -1;
    }
    case RE_NCGROUP: return bt_match(ctx, n->u.group.child, pos);
    case RE_LOOKAHEAD: return (bt_match(ctx, n->u.group.child, pos) >= 0) ? pos : -1;
    case RE_NEG_LOOKAHEAD: return (bt_match(ctx, n->u.group.child, pos) >= 0) ? -1 : pos;

    case RE_QUANTIFIER: {
        uint32_t qmin = n->u.quant.min, qmax = n->u.quant.max;
        R8EReNode *child = n->u.quant.child;
        int32_t cur = pos;
        for (uint32_t i = 0; i < qmin; i++) {
            cur = bt_match(ctx, child, cur);
            if (cur < 0) return -1;
        }
        if (n->u.quant.greedy) {
            int32_t positions[1024]; uint32_t pc = 0;
            positions[pc++] = cur;
            int32_t tp = cur; uint32_t extra = 0;
            while (extra < (qmax - qmin) && pc < 1024) {
                int32_t nx = bt_match(ctx, child, tp);
                if (nx < 0 || nx == tp) break;
                tp = nx; positions[pc++] = tp; extra++;
            }
            for (int32_t i = (int32_t)pc - 1; i >= 0; i--) return positions[i];
            return -1;
        } else {
            return cur; /* lazy: minimum first */
        }
    }
    default: return -1;
    }
}

static void bt_exec(R8ERegExp *re, const char *input, int32_t ilen,
                     int32_t start, R8EMatchResult *res) {
    res->matched = false;
    res->group_count = re->compiled.bt.group_count;

    BTCtx ctx;
    ctx.input = input; ctx.len = ilen;
    ctx.gcnt = re->compiled.bt.group_count + 1;
    ctx.re_flags = re->re_flags;

    int32_t end = (re->re_flags & R8E_RE_STICKY) ? start + 1 : ilen;

    for (int32_t pos = start; pos <= end; pos++) {
        ctx.fuel = R8E_REGEX_FUEL_MAX;
        for (uint32_t g = 0; g < R8E_RE_MAX_GROUPS; g++) { ctx.groups[g].start = -1; ctx.groups[g].end = -1; }
        int32_t r = bt_match(&ctx, re->compiled.bt.root, pos);
        if (r >= 0) {
            res->matched = true; res->match_start = pos; res->match_end = r;
            for (uint32_t g = 0; g < R8E_RE_MAX_GROUPS; g++) res->groups[g] = ctx.groups[g];
            return;
        }
        if (re->re_flags & R8E_RE_STICKY) break;
    }
}

/* ==== TIER 2: NFA64 ENGINE ==== */

static inline uint64_t nfa_eclose(R8ENFA64 *nfa, uint64_t st) {
    uint64_t prev = 0;
    while (st != prev) {
        prev = st;
        for (int i = 0; i < nfa->num_states; i++)
            if (st & ((uint64_t)1 << i)) st |= nfa->epsilon[i];
    }
    return st;
}

static inline uint64_t nfa_step(R8ENFA64 *nfa, uint64_t st, uint8_t ch) {
    uint64_t next = st & nfa->char_masks[ch];
    next = ((next << 1) | 1) & (((uint64_t)1 << nfa->num_states) - 1);
    return nfa_eclose(nfa, next);
}

static uint8_t nfa_build(R8ENFA64 *nfa, R8EReNode *n, uint8_t cur, uint32_t rf);

static uint8_t nfa_add(R8ENFA64 *nfa) { return (nfa->num_states < 63) ? nfa->num_states++ : 63; }

static void nfa_char(R8ENFA64 *nfa, uint8_t from, uint8_t ch, uint32_t rf) {
    if (rf & R8E_RE_IGNORECASE) {
        uint8_t lo = (uint8_t)to_lower(ch);
        uint8_t hi = (ch >= 'a' && ch <= 'z') ? ch - 32 : (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
        nfa->char_masks[lo] |= ((uint64_t)1 << from);
        if (hi != lo) nfa->char_masks[hi] |= ((uint64_t)1 << from);
    } else {
        nfa->char_masks[ch] |= ((uint64_t)1 << from);
    }
}

static void nfa_eps(R8ENFA64 *nfa, uint8_t f, uint8_t t) {
    if (f < 64 && t < 64) nfa->epsilon[f] |= ((uint64_t)1 << t);
}

static uint8_t nfa_build(R8ENFA64 *nfa, R8EReNode *n, uint8_t cur, uint32_t rf) {
    if (!n || nfa->num_states >= 63) return cur;
    switch (n->type) {
    case RE_LITERAL: { uint8_t nx = nfa_add(nfa); nfa_char(nfa, cur, (uint8_t)n->u.ch, rf); return nx; }
    case RE_DOT: {
        uint8_t nx = nfa_add(nfa);
        for (int c = 0; c < 256; c++) {
            if (!(rf & R8E_RE_DOTALL) && (c == '\n' || c == '\r')) continue;
            nfa->char_masks[c] |= ((uint64_t)1 << cur);
        }
        return nx;
    }
    case RE_SHORTHAND: {
        uint8_t nx = nfa_add(nfa);
        for (int c = 0; c < 256; c++)
            if (match_sh(n->u.shorthand, (uint32_t)c))
                nfa->char_masks[c] |= ((uint64_t)1 << cur);
        return nx;
    }
    case RE_CHAR_CLASS: {
        uint8_t nx = nfa_add(nfa);
        for (int c = 0; c < 256; c++)
            if (char_in_cc(n, (uint32_t)c))
                nfa->char_masks[c] |= ((uint64_t)1 << cur);
        return nx;
    }
    case RE_CONCAT: {
        uint8_t s = cur;
        for (uint32_t i = 0; i < n->u.cat.count; i++) s = nfa_build(nfa, n->u.cat.children[i], s, rf);
        return s;
    }
    case RE_ALT: {
        uint8_t ls = nfa_add(nfa); nfa_eps(nfa, cur, ls);
        uint8_t le = nfa_build(nfa, n->u.alt.left, ls, rf);
        uint8_t rs = nfa_add(nfa); nfa_eps(nfa, cur, rs);
        uint8_t re2 = nfa_build(nfa, n->u.alt.right, rs, rf);
        uint8_t join = nfa_add(nfa);
        nfa_eps(nfa, le, join); nfa_eps(nfa, re2, join);
        return join;
    }
    case RE_GROUP: case RE_NCGROUP: {
        uint8_t g = 0;
        if (n->type == RE_GROUP) { g = (uint8_t)n->u.group.gid; if (g < 32) nfa->group_starts[g] = cur; }
        uint8_t e = nfa_build(nfa, n->u.group.child, cur, rf);
        if (n->type == RE_GROUP && g < 32) { nfa->group_ends[g] = e; if (g >= nfa->group_count) nfa->group_count = g+1; }
        return e;
    }
    case RE_QUANTIFIER: {
        uint32_t qmin = n->u.quant.min, qmax = n->u.quant.max;
        uint8_t s = cur;
        for (uint32_t i = 0; i < qmin && nfa->num_states < 60; i++)
            s = nfa_build(nfa, n->u.quant.child, s, rf);
        if (qmax == UINT32_MAX) {
            uint8_t ls = s;
            uint8_t le = nfa_build(nfa, n->u.quant.child, s, rf);
            nfa_eps(nfa, le, ls);
            uint8_t after = nfa_add(nfa); nfa_eps(nfa, ls, after);
            return after;
        } else {
            for (uint32_t i = qmin; i < qmax && nfa->num_states < 60; i++) {
                uint8_t sk = nfa_add(nfa); nfa_eps(nfa, s, sk);
                s = nfa_build(nfa, n->u.quant.child, s, rf);
                nfa_eps(nfa, s, sk); s = sk;
            }
            return s;
        }
    }
    case RE_EMPTY: return cur;
    default: return cur; /* anchors/lookaheads unsupported in NFA */
    }
}

static void nfa_exec(R8ERegExp *re, const char *input, int32_t ilen,
                      int32_t start, R8EMatchResult *res) {
    R8ENFA64 *nfa = &re->compiled.nfa;
    res->matched = false; res->group_count = nfa->group_count;
    int32_t end = (re->re_flags & R8E_RE_STICKY) ? start + 1 : ilen;

    for (int32_t s = start; s <= end; s++) {
        uint64_t st = nfa_eclose(nfa, (uint64_t)1);
        int32_t best = -1;
        if (st & nfa->accept_mask) best = s;
        for (int32_t p = s; p < ilen; p++) {
            st = nfa_step(nfa, st, (uint8_t)input[p]);
            if (st == 0) break;
            if (st & nfa->accept_mask) best = p + 1;
        }
        if (best >= 0) {
            res->matched = true; res->match_start = s; res->match_end = best;
            for (uint32_t g = 0; g < R8E_RE_MAX_GROUPS; g++) { res->groups[g].start = -1; res->groups[g].end = -1; }
            res->groups[0].start = s; res->groups[0].end = best;
            return;
        }
        if (re->re_flags & R8E_RE_STICKY) break;
    }
}

/* ==== FLAGS PARSING ==== */

static int parse_flags(const char *s, uint32_t len, uint32_t *out) {
    *out = 0;
    for (uint32_t i = 0; i < len; i++) {
        switch (s[i]) {
        case 'g': *out |= R8E_RE_GLOBAL; break;
        case 'i': *out |= R8E_RE_IGNORECASE; break;
        case 'm': *out |= R8E_RE_MULTILINE; break;
        case 's': *out |= R8E_RE_DOTALL; break;
        case 'u': *out |= R8E_RE_UNICODE; break;
        case 'y': *out |= R8E_RE_STICKY; break;
        default: return -1;
        }
    }
    return 0;
}

/* ==== NODE CLEANUP ==== */

static void re_free_nodes(R8EReNode *base, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        if (base[i].type == RE_CONCAT && base[i].u.cat.children)
            free(base[i].u.cat.children);
    free(base);
}

/* ==== PUBLIC: COMPILE ==== */

R8ERegExp *r8e_regexp_compile(R8EContext *ctx, const char *pattern,
                               uint32_t pattern_len, const char *flags_str,
                               uint32_t flags_len) {
    (void)ctx;
    uint32_t rf = 0;
    if (flags_str && flags_len > 0 && parse_flags(flags_str, flags_len, &rf) < 0)
        return NULL;

    R8ERegExp *re = (R8ERegExp *)calloc(1, sizeof(R8ERegExp));
    if (!re) return NULL;
    re->flags = (R8E_GC_KIND_REGEXP << R8E_GC_KIND_SHIFT) | (1u << R8E_GC_RC_INLINE_SHIFT);
    re->proto_id = R8E_PROTO_REGEXP;
    re->re_flags = rf;

    re->pattern = (char *)malloc(pattern_len + 1);
    if (!re->pattern) { free(re); return NULL; }
    memcpy(re->pattern, pattern, pattern_len);
    re->pattern[pattern_len] = '\0';
    re->pattern_len = pattern_len;

    R8EReNodePool pool; pool.used = 0;
    ReParser parser = { pattern, pattern_len, 0, rf, &pool, 0, 0, 0, {0} };
    R8EReNode *root = re_parse_alt(&parser);

    if (parser.err) {
        for (uint32_t i = 0; i < pool.used; i++)
            if (pool.nodes[i].type == RE_CONCAT && pool.nodes[i].u.cat.children)
                free(pool.nodes[i].u.cat.children);
        free(re->pattern); free(re);
        return NULL;
    }

    if (re_is_simple(root)) {
        re->engine = 0;
        uint32_t nc = pool.used;
        R8EReNode *perm = (R8EReNode *)malloc(nc * sizeof(R8EReNode));
        if (!perm) {
            for (uint32_t i = 0; i < nc; i++)
                if (pool.nodes[i].type == RE_CONCAT && pool.nodes[i].u.cat.children)
                    free(pool.nodes[i].u.cat.children);
            free(re->pattern); free(re);
            return NULL;
        }
        memcpy(perm, pool.nodes, nc * sizeof(R8EReNode));

        /* Relocate internal pointers from pool to perm */
        ptrdiff_t off = (char *)perm - (char *)pool.nodes;
        #define RELOC(ptr) do { if (ptr) ptr = (R8EReNode *)((char *)(ptr) + off); } while(0)
        for (uint32_t i = 0; i < nc; i++) {
            R8EReNode *n = &perm[i];
            switch (n->type) {
            case RE_QUANTIFIER: RELOC(n->u.quant.child); break;
            case RE_GROUP: case RE_NCGROUP: case RE_LOOKAHEAD: case RE_NEG_LOOKAHEAD:
                RELOC(n->u.group.child); break;
            case RE_ALT: RELOC(n->u.alt.left); RELOC(n->u.alt.right); break;
            case RE_CONCAT:
                for (uint32_t j = 0; j < n->u.cat.count; j++) RELOC(n->u.cat.children[j]);
                break;
            default: break;
            }
        }
        #undef RELOC

        re->compiled.bt.root = (R8EReNode *)((char *)root + off);
        re->compiled.bt.block = perm; /* block base for correct free */
        re->compiled.bt.node_count = nc;
        re->compiled.bt.group_count = parser.groups;
        /* Prevent double-free: concat children now owned by perm */
        for (uint32_t i = 0; i < pool.used; i++) pool.nodes[i].u.cat.children = NULL;
    } else {
        re->engine = 1;
        memset(&re->compiled.nfa, 0, sizeof(R8ENFA64));
        re->compiled.nfa.num_states = 1;
        uint8_t accept = nfa_build(&re->compiled.nfa, root, 0, rf);
        re->compiled.nfa.accept_mask = (uint64_t)1 << accept;
        for (uint32_t i = 0; i < pool.used; i++)
            if (pool.nodes[i].type == RE_CONCAT && pool.nodes[i].u.cat.children)
                free(pool.nodes[i].u.cat.children);
    }
    return re;
}

/* ==== PUBLIC: EXEC ==== */

void r8e_regexp_exec(R8ERegExp *re, const char *input, int32_t input_len,
                      R8EMatchResult *result) {
    if (!re || !input || !result) { if (result) result->matched = false; return; }
    int32_t sp = 0;
    if (re->re_flags & (R8E_RE_GLOBAL | R8E_RE_STICKY)) {
        sp = (int32_t)re->last_index;
        if (sp > input_len) { result->matched = false; re->last_index = 0; return; }
    }
    if (re->engine == 0) bt_exec(re, input, input_len, sp, result);
    else nfa_exec(re, input, input_len, sp, result);

    if (result->matched && (re->re_flags & (R8E_RE_GLOBAL | R8E_RE_STICKY)))
        re->last_index = (uint32_t)result->match_end;
    else if (!result->matched && (re->re_flags & (R8E_RE_GLOBAL | R8E_RE_STICKY)))
        re->last_index = 0;
}

/* ==== PUBLIC: TEST ==== */

bool r8e_regexp_test(R8ERegExp *re, const char *input, int32_t input_len) {
    R8EMatchResult res;
    r8e_regexp_exec(re, input, input_len, &res);
    return res.matched;
}

/* ==== GC SUPPORT ==== */

void r8e_regexp_gc_mark(R8ERegExp *re) { if (re) re->flags |= R8E_GC_MARK; }

void r8e_regexp_gc_free(R8ERegExp *re) {
    if (!re) return;
    free(re->pattern);
    if (re->engine == 0 && re->compiled.bt.block)
        re_free_nodes(re->compiled.bt.block, re->compiled.bt.node_count);
    free(re);
}

/* ==== BUILT-IN JS BINDINGS ==== */

/* Helper: extract string pointer and length from R8EValue */
static bool extract_str(R8EValue v, char *ibuf, const char **out, int32_t *olen) {
    if (R8E_IS_INLINE_STR(v)) {
        int len = (int)((v >> 45) & 0x7);
        for (int i = 0; i < len; i++) ibuf[i] = (char)((v >> (38 - i * 7)) & 0x7F);
        ibuf[len] = '\0'; *out = ibuf; *olen = len;
        return true;
    }
    if (R8E_IS_POINTER(v)) {
        uint8_t *ptr = (uint8_t *)r8e_get_pointer(v);
        if (!ptr) return false;
        uint32_t bl; memcpy(&bl, ptr + 8, 4);
        *out = (const char *)(ptr + 20); *olen = (int32_t)bl;
        return true;
    }
    return false;
}

/* Helper: encode short string as inline NaN-boxed value */
static R8EValue make_inline(const char *s, int len) {
    uint64_t v = 0xFFFD000000000000ULL;
    v |= ((uint64_t)(unsigned)len << 45);
    for (int i = 0; i < len; i++) v |= ((uint64_t)(uint8_t)s[i] << (38 - i * 7));
    return v;
}

R8EValue r8e_builtin_regexp_exec(R8EContext *ctx, R8EValue this_val,
                                  int argc, const R8EValue *argv) {
    (void)ctx;
    if (!R8E_IS_POINTER(this_val) || argc < 1) return R8E_NULL;
    R8ERegExp *re = (R8ERegExp *)r8e_get_pointer(this_val);
    if (!re) return R8E_NULL;

    char ibuf[8]; const char *input; int32_t ilen;
    if (!extract_str(argv[0], ibuf, &input, &ilen)) return R8E_NULL;

    R8EMatchResult res;
    r8e_regexp_exec(re, input, ilen, &res);
    return res.matched ? r8e_from_int32(res.match_start) : R8E_NULL;
}

R8EValue r8e_builtin_regexp_test(R8EContext *ctx, R8EValue this_val,
                                  int argc, const R8EValue *argv) {
    R8EValue r = r8e_builtin_regexp_exec(ctx, this_val, argc, argv);
    return (r != R8E_NULL) ? R8E_TRUE : R8E_FALSE;
}

R8EValue r8e_builtin_regexp_toString(R8EContext *ctx, R8EValue this_val,
                                      int argc, const R8EValue *argv) {
    (void)ctx; (void)argc; (void)argv;
    if (!R8E_IS_POINTER(this_val)) return R8E_UNDEFINED;
    R8ERegExp *re = (R8ERegExp *)r8e_get_pointer(this_val);
    if (!re) return R8E_UNDEFINED;

    char fb[7]; int fi = 0;
    if (re->re_flags & R8E_RE_GLOBAL)     fb[fi++] = 'g';
    if (re->re_flags & R8E_RE_IGNORECASE) fb[fi++] = 'i';
    if (re->re_flags & R8E_RE_MULTILINE)  fb[fi++] = 'm';
    if (re->re_flags & R8E_RE_DOTALL)     fb[fi++] = 's';
    if (re->re_flags & R8E_RE_UNICODE)    fb[fi++] = 'u';
    if (re->re_flags & R8E_RE_STICKY)     fb[fi++] = 'y';

    uint32_t tl = 1 + re->pattern_len + 1 + (uint32_t)fi;
    if (tl <= 7) {
        char buf[8]; int p = 0;
        buf[p++] = '/';
        memcpy(buf + p, re->pattern, re->pattern_len); p += (int)re->pattern_len;
        buf[p++] = '/';
        memcpy(buf + p, fb, fi); p += fi;
        return make_inline(buf, p);
    }
    return R8E_UNDEFINED; /* heap string needed for long patterns */
}

/* ==== STRING INTEGRATION STUBS ==== */

R8EValue r8e_regexp_string_match(R8EContext *ctx, R8EValue str, R8EValue rxv) {
    (void)ctx;
    if (!R8E_IS_POINTER(rxv)) return R8E_NULL;
    R8ERegExp *re = (R8ERegExp *)r8e_get_pointer(rxv);
    if (!re) return R8E_NULL;

    char ibuf[8]; const char *input; int32_t ilen;
    if (!extract_str(str, ibuf, &input, &ilen)) return R8E_NULL;

    R8EMatchResult res;
    if (!(re->re_flags & R8E_RE_GLOBAL)) {
        r8e_regexp_exec(re, input, ilen, &res);
        return res.matched ? r8e_from_int32(res.match_start) : R8E_NULL;
    }

    uint32_t saved = re->last_index; re->last_index = 0;
    int32_t cnt = 0;
    for (;;) {
        r8e_regexp_exec(re, input, ilen, &res);
        if (!res.matched) break;
        cnt++;
        if (res.match_start == res.match_end)
            re->last_index = (uint32_t)res.match_end + 1;
    }
    re->last_index = saved;
    return cnt > 0 ? r8e_from_int32(cnt) : R8E_NULL;
}

R8EValue r8e_regexp_string_search(R8EContext *ctx, R8EValue str, R8EValue rxv) {
    (void)ctx;
    if (!R8E_IS_POINTER(rxv)) return r8e_from_int32(-1);
    R8ERegExp *re = (R8ERegExp *)r8e_get_pointer(rxv);
    if (!re) return r8e_from_int32(-1);

    char ibuf[8]; const char *input; int32_t ilen;
    if (!extract_str(str, ibuf, &input, &ilen)) return r8e_from_int32(-1);

    uint32_t sl = re->last_index, sf = re->re_flags;
    re->last_index = 0; re->re_flags &= ~(R8E_RE_GLOBAL | R8E_RE_STICKY);
    R8EMatchResult res;
    r8e_regexp_exec(re, input, ilen, &res);
    re->last_index = sl; re->re_flags = sf;
    return res.matched ? r8e_from_int32(res.match_start) : r8e_from_int32(-1);
}

R8EValue r8e_regexp_string_replace(R8EContext *ctx, R8EValue str,
                                    R8EValue rxv, R8EValue repl) {
    (void)ctx;
    if (!R8E_IS_POINTER(rxv)) return str;
    R8ERegExp *re = (R8ERegExp *)r8e_get_pointer(rxv);
    if (!re) return str;

    char ibuf[8]; const char *input; int32_t ilen;
    if (!extract_str(str, ibuf, &input, &ilen)) return str;
    char rbuf[8]; const char *rs; int32_t rlen;
    if (!extract_str(repl, rbuf, &rs, &rlen)) return str;

    int32_t ocap = ilen + rlen * 4 + 64;
    char *out = (char *)malloc((size_t)ocap);
    if (!out) return str;
    int32_t olen = 0;

    uint32_t saved = re->last_index; re->last_index = 0;
    int32_t last_end = 0;
    bool global = (re->re_flags & R8E_RE_GLOBAL) != 0;

    for (;;) {
        R8EMatchResult res;
        r8e_regexp_exec(re, input, ilen, &res);
        if (!res.matched) break;

        int32_t blen = res.match_start - last_end;
        if (olen + blen + rlen + 1 > ocap) {
            ocap = (olen + blen + rlen + 1) * 2;
            char *nw = (char *)realloc(out, (size_t)ocap);
            if (!nw) { free(out); re->last_index = saved; return str; }
            out = nw;
        }
        memcpy(out + olen, input + last_end, (size_t)blen); olen += blen;
        memcpy(out + olen, rs, (size_t)rlen); olen += rlen;
        last_end = res.match_end;

        if (res.match_start == res.match_end) {
            re->last_index = (uint32_t)res.match_end + 1;
            if (last_end < ilen) out[olen++] = input[last_end++];
        }
        if (!global) break;
    }

    int32_t rem = ilen - last_end;
    if (olen + rem + 1 > ocap) {
        char *nw = (char *)realloc(out, (size_t)(olen + rem + 1));
        if (!nw) { free(out); re->last_index = saved; return str; }
        out = nw;
    }
    memcpy(out + olen, input + last_end, (size_t)rem); olen += rem;
    out[olen] = '\0';
    re->last_index = saved;

    if (olen <= 7) {
        bool ascii = true;
        for (int32_t i = 0; i < olen; i++) if ((uint8_t)out[i] > 127) { ascii = false; break; }
        if (ascii) { R8EValue v = make_inline(out, olen); free(out); return v; }
    }
    free(out);
    return str; /* heap string allocation needed for full integration */
}

R8EValue r8e_regexp_string_split(R8EContext *ctx, R8EValue str,
                                  R8EValue rxv, int32_t limit) {
    (void)ctx;
    if (!R8E_IS_POINTER(rxv)) return r8e_from_int32(0);
    R8ERegExp *re = (R8ERegExp *)r8e_get_pointer(rxv);
    if (!re) return r8e_from_int32(0);

    char ibuf[8]; const char *input; int32_t ilen;
    if (!extract_str(str, ibuf, &input, &ilen)) return r8e_from_int32(0);

    if (limit == 0) return r8e_from_int32(0);
    if (limit < 0) limit = INT32_MAX;

    uint32_t saved = re->last_index; re->last_index = 0;
    int32_t cnt = 0, last_end = 0;

    while (cnt < limit - 1) {
        R8EMatchResult res;
        r8e_regexp_exec(re, input, ilen, &res);
        if (!res.matched) break;
        if (res.match_end == last_end && res.match_start == last_end) {
            re->last_index = (uint32_t)last_end + 1;
            if (last_end >= ilen) break;
            continue;
        }
        cnt++; last_end = res.match_end;
    }
    cnt++;
    re->last_index = saved;
    return r8e_from_int32(cnt);
}

/* ==== PROPERTY ACCESSOR ==== */

R8EValue r8e_regexp_get_prop(R8ERegExp *re, uint32_t atom) {
    if (!re) return R8E_UNDEFINED;
    switch (atom) {
    case R8E_ATOM_source:
        if (re->pattern_len <= 7) {
            bool ok = true;
            for (uint32_t i = 0; i < re->pattern_len; i++) if ((uint8_t)re->pattern[i] > 127) { ok = false; break; }
            if (ok) return make_inline(re->pattern, (int)re->pattern_len);
        }
        return R8E_UNDEFINED;
    case R8E_ATOM_flags: {
        char b[7]; int f = 0;
        if (re->re_flags & R8E_RE_GLOBAL)     b[f++] = 'g';
        if (re->re_flags & R8E_RE_IGNORECASE) b[f++] = 'i';
        if (re->re_flags & R8E_RE_MULTILINE)  b[f++] = 'm';
        if (re->re_flags & R8E_RE_DOTALL)     b[f++] = 's';
        if (re->re_flags & R8E_RE_UNICODE)    b[f++] = 'u';
        if (re->re_flags & R8E_RE_STICKY)     b[f++] = 'y';
        return make_inline(b, f);
    }
    case R8E_ATOM_global:     return r8e_from_bool(re->re_flags & R8E_RE_GLOBAL);
    case R8E_ATOM_ignoreCase: return r8e_from_bool(re->re_flags & R8E_RE_IGNORECASE);
    case R8E_ATOM_multiline:  return r8e_from_bool(re->re_flags & R8E_RE_MULTILINE);
    case R8E_ATOM_dotAll:     return r8e_from_bool(re->re_flags & R8E_RE_DOTALL);
    case R8E_ATOM_unicode:    return r8e_from_bool(re->re_flags & R8E_RE_UNICODE);
    case R8E_ATOM_sticky:     return r8e_from_bool(re->re_flags & R8E_RE_STICKY);
    case R8E_ATOM_lastIndex:  return r8e_from_int32((int32_t)re->last_index);
    default: return R8E_UNDEFINED;
    }
}

R8EValue r8e_regexp_set_prop(R8ERegExp *re, uint32_t atom, R8EValue val) {
    if (!re) return R8E_FALSE;
    if (atom == R8E_ATOM_lastIndex && R8E_IS_INT32(val)) {
        int32_t v = r8e_get_int32(val);
        re->last_index = (v >= 0) ? (uint32_t)v : 0;
        return R8E_TRUE;
    }
    return R8E_FALSE;
}

/* ==== CONSTRUCTOR HELPER ==== */

R8EValue r8e_regexp_construct(R8EContext *ctx, const char *pattern,
                               uint32_t pattern_len, const char *flags,
                               uint32_t flags_len) {
    R8ERegExp *re = r8e_regexp_compile(ctx, pattern, pattern_len, flags, flags_len);
    return re ? r8e_from_pointer(re) : R8E_UNDEFINED;
}

/* ==== CONVENIENCE: quick_test / quick_search ==== */

static void flags_to_str(uint32_t rf, char *buf, int *fi) {
    *fi = 0;
    if (rf & R8E_RE_GLOBAL)     buf[(*fi)++] = 'g';
    if (rf & R8E_RE_IGNORECASE) buf[(*fi)++] = 'i';
    if (rf & R8E_RE_MULTILINE)  buf[(*fi)++] = 'm';
    if (rf & R8E_RE_DOTALL)     buf[(*fi)++] = 's';
    if (rf & R8E_RE_UNICODE)    buf[(*fi)++] = 'u';
    if (rf & R8E_RE_STICKY)     buf[(*fi)++] = 'y';
}

bool r8e_regexp_quick_test(const char *pattern, uint32_t plen,
                            const char *input, int32_t ilen, uint32_t rf) {
    char fb[7]; int fi; flags_to_str(rf, fb, &fi);
    R8ERegExp *re = r8e_regexp_compile(NULL, pattern, plen, fb, (uint32_t)fi);
    if (!re) return false;
    bool r = r8e_regexp_test(re, input, ilen);
    r8e_regexp_gc_free(re);
    return r;
}

int32_t r8e_regexp_quick_search(const char *pattern, uint32_t plen,
                                 const char *input, int32_t ilen, uint32_t rf) {
    char fb[7]; int fi; flags_to_str(rf, fb, &fi);
    R8ERegExp *re = r8e_regexp_compile(NULL, pattern, plen, fb, (uint32_t)fi);
    if (!re) return -1;
    R8EMatchResult res;
    r8e_regexp_exec(re, input, ilen, &res);
    int32_t pos = res.matched ? res.match_start : -1;
    r8e_regexp_gc_free(re);
    return pos;
}
