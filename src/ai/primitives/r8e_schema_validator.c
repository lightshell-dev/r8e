/*
 * r8e_schema_validator.c - Compiled JSON Schema Validator for AI Tool Calls
 *
 * Two-phase: compile JSON Schema to bytecode, validate JSON against it.
 * Validate phase does zero heap allocation.
 *
 * Copyright (c) 2026 r8e Authors. All rights reserved.
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "r8e_schema_validator.h"

/* --- Bytecode ops --- */
enum {
    OP_CHECK_TYPE = 0x01, OP_CHECK_REQUIRED, OP_ENTER_OBJECT, OP_EXIT_OBJECT,
    OP_ENTER_ARRAY, OP_EXIT_ARRAY, OP_ITER_PROPERTIES, OP_ITER_ITEMS,
    OP_CHECK_STR_LEN, OP_CHECK_NUM_RANGE, OP_CHECK_ENUM, OP_CHECK_ARRAY_LEN,
    OP_NO_ADDITIONAL, OP_ENTER_PROP, OP_EXIT_PROP, OP_BRANCH_ANYOF,
    OP_BRANCH_ONEOF, OP_DONE = 0xFF
};

/* Type mask bits */
#define TM_OBJ 0x01
#define TM_ARR 0x02
#define TM_STR 0x04
#define TM_NUM 0x08
#define TM_INT 0x10
#define TM_BOOL 0x20
#define TM_NULL 0x40

/* --- Compiled validator --- */
struct R8ESchemaValidator {
    uint8_t *bc;
    int      len, cap;
};

/* --- JSON scanner (shared by both phases) --- */
typedef struct { const char *s; int p, n; } JS;

static void js_ws(JS *j) {
    while (j->p < j->n && (j->s[j->p]==' '||j->s[j->p]=='\t'||
           j->s[j->p]=='\n'||j->s[j->p]=='\r')) j->p++;
}
static char js_peek(JS *j) { js_ws(j); return j->p<j->n ? j->s[j->p] : 0; }
static bool js_eat(JS *j, char c) {
    js_ws(j); if (j->p<j->n && j->s[j->p]==c) { j->p++; return true; } return false;
}

static const char *js_str(JS *j, int *len) {
    js_ws(j);
    if (j->p>=j->n || j->s[j->p]!='"') return NULL;
    j->p++;
    int st = j->p;
    while (j->p<j->n && j->s[j->p]!='"') { if (j->s[j->p]=='\\') j->p++; j->p++; }
    *len = j->p - st;
    if (j->p<j->n) j->p++;
    return j->s + st;
}

static double js_num(JS *j) {
    js_ws(j); int st = j->p;
    if (j->p<j->n && j->s[j->p]=='-') j->p++;
    while (j->p<j->n && j->s[j->p]>='0' && j->s[j->p]<='9') j->p++;
    if (j->p<j->n && j->s[j->p]=='.') { j->p++;
        while (j->p<j->n && j->s[j->p]>='0' && j->s[j->p]<='9') j->p++; }
    if (j->p<j->n && (j->s[j->p]=='e'||j->s[j->p]=='E')) { j->p++;
        if (j->p<j->n && (j->s[j->p]=='+'||j->s[j->p]=='-')) j->p++;
        while (j->p<j->n && j->s[j->p]>='0' && j->s[j->p]<='9') j->p++; }
    char buf[64]; int l = j->p-st; if (l>63) l=63;
    memcpy(buf, j->s+st, l); buf[l]=0; return strtod(buf, NULL);
}

static bool js_bool(JS *j) {
    js_ws(j);
    if (j->p+4<=j->n && !memcmp(j->s+j->p,"true",4)) { j->p+=4; return true; }
    if (j->p+5<=j->n && !memcmp(j->s+j->p,"false",5)) { j->p+=5; return false; }
    return false;
}

static void js_skip(JS *j) {
    js_ws(j); if (j->p>=j->n) return;
    char c = j->s[j->p];
    if (c=='"') { int l; js_str(j,&l); }
    else if (c=='{') {
        j->p++; js_ws(j);
        if (j->p<j->n && j->s[j->p]=='}') { j->p++; return; }
        for (;;) { int l; js_str(j,&l); js_eat(j,':'); js_skip(j); if (!js_eat(j,',')) break; }
        js_eat(j,'}');
    } else if (c=='[') {
        j->p++; js_ws(j);
        if (j->p<j->n && j->s[j->p]==']') { j->p++; return; }
        for (;;) { js_skip(j); if (!js_eat(j,',')) break; }
        js_eat(j,']');
    } else if (c=='t') j->p+=4;
    else if (c=='f') j->p+=5;
    else if (c=='n') j->p+=4;
    else js_num(j);
}

static bool streq(const char *a, int al, const char *b) {
    int bl=(int)strlen(b); return al==bl && !memcmp(a,b,al);
}

/* --- Bytecode emitter --- */
static void em8(R8ESchemaValidator *v, uint8_t b) {
    if (v->len+1 > v->cap) {
        int c = v->cap ? v->cap*2 : 256;
        v->bc = realloc(v->bc, c); if (!v->bc) return; v->cap = c;
    }
    v->bc[v->len++] = b;
}
static void em16(R8ESchemaValidator *v, uint16_t w) { em8(v,w&0xFF); em8(v,w>>8); }
static void em32(R8ESchemaValidator *v, int32_t i) {
    uint32_t u; memcpy(&u,&i,4);
    em8(v,u&0xFF); em8(v,(u>>8)&0xFF); em8(v,(u>>16)&0xFF); em8(v,(u>>24)&0xFF);
}
static void emf64(R8ESchemaValidator *v, double d) {
    uint8_t b[8]; memcpy(b,&d,8); for (int i=0;i<8;i++) em8(v,b[i]);
}
static void embytes(R8ESchemaValidator *v, const char *s, int l) {
    for (int i=0;i<l;i++) em8(v,(uint8_t)s[i]);
}
static void patch16(R8ESchemaValidator *v, int off, uint16_t w) {
    if (off+1<v->len) { v->bc[off]=w&0xFF; v->bc[off+1]=w>>8; }
}

/* --- Schema compiler --- */
static uint8_t type_mask(const char *s, int l) {
    if (streq(s,l,"object"))  return TM_OBJ;
    if (streq(s,l,"array"))   return TM_ARR;
    if (streq(s,l,"string"))  return TM_STR;
    if (streq(s,l,"number"))  return TM_NUM;
    if (streq(s,l,"integer")) return TM_INT;
    if (streq(s,l,"boolean")) return TM_BOOL;
    if (streq(s,l,"null"))    return TM_NULL;
    return 0;
}

static void compile(R8ESchemaValidator *v, JS *r) {
    if (!js_eat(r,'{')) return;
    if (js_peek(r)=='}') { js_eat(r,'}'); return; }

    uint8_t tmask = 0; bool has_type = false;
    const char *rk[64]; int rl[64]; int rc_ = 0;
    struct { const char *k; int kl, sp; } props[64]; int pc_ = 0;
    int items_pos = -1;
    const char *ev[64]; int el[64]; int ec_ = 0;
    int32_t minL=-1, maxL=-1, minI=-1, maxI=-1;
    double mn=NAN, mx=NAN;
    bool no_add=false, has_add=false;
    const char *pk[64]; int pkl[64];
    int bp[32]; int bc_=0; uint8_t bop=0;

    for (;;) {
        int kl; const char *k = js_str(r,&kl); if (!k) break;
        js_eat(r,':');
        if (streq(k,kl,"type")) {
            has_type = true;
            if (js_peek(r)=='"') { int tl; const char *t=js_str(r,&tl); tmask=type_mask(t,tl); }
            else if (js_peek(r)=='[') {
                js_eat(r,'[');
                for (;;) { int tl; const char *t=js_str(r,&tl); if(t) tmask|=type_mask(t,tl);
                    if (!js_eat(r,',')) break; }
                js_eat(r,']');
            } else js_skip(r);
        } else if (streq(k,kl,"properties")) {
            js_eat(r,'{');
            while (js_peek(r)=='"') {
                int pl; const char *pk_=js_str(r,&pl); js_eat(r,':');
                if (pc_<64) { props[pc_].k=pk_; props[pc_].kl=pl; props[pc_].sp=r->p;
                    pk[pc_]=pk_; pkl[pc_]=pl; pc_++; }
                js_skip(r); js_eat(r,',');
            }
            js_eat(r,'}');
        } else if (streq(k,kl,"required")) {
            js_eat(r,'[');
            while (js_peek(r)=='"') {
                int sl; const char *s=js_str(r,&sl);
                if (s && rc_<64) { rk[rc_]=s; rl[rc_]=sl; rc_++; }
                js_eat(r,',');
            }
            js_eat(r,']');
        } else if (streq(k,kl,"items")) {
            items_pos = r->p; js_skip(r);
        } else if (streq(k,kl,"enum")) {
            js_eat(r,'[');
            while (js_peek(r)!=']' && r->p<r->n) {
                js_ws(r);
                if (js_peek(r)=='"') {
                    int sl; const char *s=js_str(r,&sl);
                    if (s && ec_<64) { ev[ec_]=s; el[ec_]=sl; ec_++; }
                } else { int st=r->p; js_skip(r);
                    if (ec_<64) { ev[ec_]=r->s+st; el[ec_]=r->p-st; ec_++; } }
                js_eat(r,',');
            }
            js_eat(r,']');
        } else if (streq(k,kl,"minLength")) minL=(int32_t)js_num(r);
        else if (streq(k,kl,"maxLength")) maxL=(int32_t)js_num(r);
        else if (streq(k,kl,"minimum")) mn=js_num(r);
        else if (streq(k,kl,"maximum")) mx=js_num(r);
        else if (streq(k,kl,"minItems")) minI=(int32_t)js_num(r);
        else if (streq(k,kl,"maxItems")) maxI=(int32_t)js_num(r);
        else if (streq(k,kl,"additionalProperties")) { has_add=true; no_add=!js_bool(r); }
        else if (streq(k,kl,"anyOf")||streq(k,kl,"oneOf")) {
            bop = streq(k,kl,"anyOf") ? OP_BRANCH_ANYOF : OP_BRANCH_ONEOF;
            js_eat(r,'[');
            while (js_peek(r)=='{') { if (bc_<32) bp[bc_++]=r->p; js_skip(r); js_eat(r,','); }
            js_eat(r,']');
        } else js_skip(r); /* description, unknown keys */
        if (!js_eat(r,',')) break;
    }
    js_eat(r,'}');

    /* Emit bytecode */
    if (has_type) { em8(v,OP_CHECK_TYPE); em8(v,tmask); }
    if (minL>=0||maxL>=0) { em8(v,OP_CHECK_STR_LEN); em32(v,minL); em32(v,maxL); }
    if (!isnan(mn)||!isnan(mx)) { em8(v,OP_CHECK_NUM_RANGE); emf64(v,mn); emf64(v,mx); }
    if (ec_>0) { em8(v,OP_CHECK_ENUM); em16(v,(uint16_t)ec_);
        for (int i=0;i<ec_;i++) { em16(v,(uint16_t)el[i]); embytes(v,ev[i],el[i]); } }
    if (minI>=0||maxI>=0) { em8(v,OP_CHECK_ARRAY_LEN); em32(v,minI); em32(v,maxI); }
    for (int i=0;i<rc_;i++) { em8(v,OP_CHECK_REQUIRED); em16(v,(uint16_t)rl[i]); embytes(v,rk[i],rl[i]); }
    if (pc_>0) {
        em8(v,OP_ENTER_OBJECT);
        for (int i=0;i<pc_;i++) {
            em8(v,OP_ENTER_PROP); em16(v,(uint16_t)props[i].kl);
            embytes(v,props[i].k,props[i].kl);
            int sp=v->len; em16(v,0);
            JS sub={r->s,props[i].sp,r->n}; compile(v,&sub);
            em8(v,OP_EXIT_PROP); patch16(v,sp,(uint16_t)v->len);
        }
        em8(v,OP_EXIT_OBJECT);
    }
    if (has_add && no_add && pc_>0) {
        em8(v,OP_NO_ADDITIONAL); em16(v,(uint16_t)pc_);
        for (int i=0;i<pc_;i++) { em16(v,(uint16_t)pkl[i]); embytes(v,pk[i],pkl[i]); }
    }
    if (items_pos>=0) {
        em8(v,OP_ENTER_ARRAY); em8(v,OP_ITER_ITEMS);
        int sp=v->len; em16(v,0);
        JS sub={r->s,items_pos,r->n}; compile(v,&sub);
        em8(v,OP_EXIT_ARRAY); patch16(v,sp,(uint16_t)v->len);
    }
    if (bc_>0 && bop) {
        em8(v,bop); em16(v,(uint16_t)bc_);
        int ts=v->len; for (int i=0;i<bc_;i++) em16(v,0);
        for (int i=0;i<bc_;i++) {
            int bs=v->len; JS sub={r->s,bp[i],r->n}; compile(v,&sub);
            patch16(v,ts+i*2,(uint16_t)(v->len-bs));
        }
    }
}

/* --- JSON value type detection (validate phase) --- */
typedef enum { JT_NONE,JT_OBJ,JT_ARR,JT_STR,JT_NUM,JT_TRUE,JT_FALSE,JT_NULL } JType;

static JType jtype(JS *j) {
    js_ws(j); if (j->p>=j->n) return JT_NONE;
    char c=j->s[j->p];
    if (c=='{') return JT_OBJ; if (c=='[') return JT_ARR; if (c=='"') return JT_STR;
    if (c=='t') return JT_TRUE; if (c=='f') return JT_FALSE; if (c=='n') return JT_NULL;
    if (c=='-'||(c>='0'&&c<='9')) return JT_NUM;
    return JT_NONE;
}

static bool jfind(JS *j, const char *key, int kl) {
    int sv=j->p; js_ws(j);
    if (j->p>=j->n||j->s[j->p]!='{') { j->p=sv; return false; }
    j->p++; js_ws(j);
    if (j->p<j->n&&j->s[j->p]=='}') { j->p=sv; return false; }
    for (;;) {
        int sl; const char *s=js_str(j,&sl); if (!s) break;
        js_ws(j); if (j->p<j->n&&j->s[j->p]==':') j->p++; js_ws(j);
        if (sl==kl && !memcmp(s,key,kl)) return true;
        js_skip(j); js_ws(j);
        if (j->p<j->n&&j->s[j->p]==',') j->p++; else break;
    }
    j->p=sv; return false;
}

static bool jhas(JS *j, const char *key, int kl) {
    int sv=j->p; bool f=jfind(j,key,kl); j->p=sv; return f;
}

static int jstrlen(JS *j) {
    int sv=j->p; js_ws(j);
    if (j->p>=j->n||j->s[j->p]!='"') { j->p=sv; return 0; }
    j->p++; int l=0;
    while (j->p<j->n&&j->s[j->p]!='"') {
        if (j->s[j->p]=='\\') { j->p++;
            if (j->p<j->n&&j->s[j->p]=='u') j->p+=4; else j->p++; }
        else j->p++;
        l++;
    }
    j->p=sv; return l;
}

static const char *jpeekstr(JS *j, int *len) {
    int sv=j->p; js_ws(j);
    if (j->p>=j->n||j->s[j->p]!='"') { j->p=sv; return NULL; }
    j->p++; const char *st=j->s+j->p; int l=0;
    while (j->p<j->n&&j->s[j->p]!='"') {
        if (j->s[j->p]=='\\') { j->p++; l++; } j->p++; l++;
    }
    *len=l; j->p=sv; return st;
}

static int jarrlen(JS *j) {
    int sv=j->p; js_ws(j);
    if (j->p>=j->n||j->s[j->p]!='[') { j->p=sv; return 0; }
    j->p++; js_ws(j);
    if (j->p<j->n&&j->s[j->p]==']') { j->p=sv; return 0; }
    int c=0;
    for (;;) { js_skip(j); c++; js_ws(j);
        if (j->p<j->n&&j->s[j->p]==',') j->p++; else break; }
    j->p=sv; return c;
}

static bool numisint(const char *s, int st, int en) {
    for (int i=st;i<en;i++) if (s[i]=='.') return false;
    return true;
}

/* --- Bytecode reader helpers --- */
static uint8_t rd8(const uint8_t *b, int *p) { return b[(*p)++]; }
static uint16_t rd16(const uint8_t *b, int *p) {
    uint16_t lo=b[(*p)++], hi=b[(*p)++]; return lo|(hi<<8);
}
static int32_t rd32(const uint8_t *b, int *p) {
    uint32_t u=0; for(int i=0;i<4;i++) u|=(uint32_t)b[(*p)++]<<(i*8);
    int32_t r; memcpy(&r,&u,4); return r;
}
static double rdf64(const uint8_t *b, int *p) {
    double d; memcpy(&d,b+*p,8); *p+=8; return d;
}

/* --- Bytecode interpreter --- */
static int vrun(const uint8_t *bc, int bl, int *pc, JS *n, const char **em) {
    while (*pc < bl) {
        uint8_t op = rd8(bc, pc);
        switch (op) {
        case OP_DONE: return 0;
        case OP_CHECK_TYPE: {
            uint8_t m = rd8(bc,pc); JType t = jtype(n); bool ok=false;
            if ((m&TM_OBJ)&&t==JT_OBJ) ok=true;
            if ((m&TM_ARR)&&t==JT_ARR) ok=true;
            if ((m&TM_STR)&&t==JT_STR) ok=true;
            if ((m&TM_NUM)&&t==JT_NUM) ok=true;
            if ((m&TM_INT)&&t==JT_NUM) {
                JS tmp=*n; js_ws(&tmp); int ns=tmp.p;
                js_skip(&tmp); if (numisint(n->s,ns,tmp.p)) ok=true;
            }
            if ((m&TM_BOOL)&&(t==JT_TRUE||t==JT_FALSE)) ok=true;
            if ((m&TM_NULL)&&t==JT_NULL) ok=true;
            if (!ok) { if(em) *em="type mismatch"; return R8E_SCHEMA_ERR_TYPE; }
            break; }
        case OP_CHECK_REQUIRED: {
            uint16_t kl=rd16(bc,pc); const char *k=(const char*)(bc+*pc); *pc+=kl;
            if (!jhas(n,k,kl)) { if(em) *em="missing required property"; return R8E_SCHEMA_ERR_REQUIRED; }
            break; }
        case OP_CHECK_STR_LEN: {
            int32_t mi=rd32(bc,pc), ma=rd32(bc,pc);
            if (jtype(n)==JT_STR) { int sl=jstrlen(n);
                if (mi>=0&&sl<mi) { if(em) *em="string too short"; return R8E_SCHEMA_ERR_STRING_LEN; }
                if (ma>=0&&sl>ma) { if(em) *em="string too long"; return R8E_SCHEMA_ERR_STRING_LEN; } }
            break; }
        case OP_CHECK_NUM_RANGE: {
            double mi=rdf64(bc,pc), ma=rdf64(bc,pc);
            if (jtype(n)==JT_NUM) { int sv=n->p; double v=js_num(n); n->p=sv;
                if (!isnan(mi)&&v<mi) { if(em) *em="number below minimum"; return R8E_SCHEMA_ERR_NUM_RANGE; }
                if (!isnan(ma)&&v>ma) { if(em) *em="number above maximum"; return R8E_SCHEMA_ERR_NUM_RANGE; } }
            break; }
        case OP_CHECK_ENUM: {
            uint16_t cnt=rd16(bc,pc); bool matched=false; JType t=jtype(n);
            if (t==JT_STR) { int sl; const char *sv=jpeekstr(n,&sl);
                for (uint16_t i=0;i<cnt;i++) { uint16_t el=rd16(bc,pc);
                    const char *ev=(const char*)(bc+*pc); *pc+=el;
                    if (sv&&sl==el&&!memcmp(sv,ev,el)) matched=true; }
            } else { for (uint16_t i=0;i<cnt;i++) { uint16_t el=rd16(bc,pc); *pc+=el; } }
            if (!matched&&t==JT_STR) { if(em) *em="value not in enum"; return R8E_SCHEMA_ERR_ENUM; }
            break; }
        case OP_CHECK_ARRAY_LEN: {
            int32_t mi=rd32(bc,pc), ma=rd32(bc,pc);
            if (jtype(n)==JT_ARR) { int c=jarrlen(n);
                if (mi>=0&&c<mi) { if(em) *em="array too short"; return R8E_SCHEMA_ERR_ARRAY_LEN; }
                if (ma>=0&&c>ma) { if(em) *em="array too long"; return R8E_SCHEMA_ERR_ARRAY_LEN; } }
            break; }
        case OP_ENTER_OBJECT: case OP_EXIT_OBJECT: break;
        case OP_ENTER_PROP: {
            uint16_t kl=rd16(bc,pc); const char *key=(const char*)(bc+*pc); *pc+=kl;
            uint16_t skip=rd16(bc,pc); int sv=n->p;
            if (jfind(n,key,kl)) {
                int r=vrun(bc,bl,pc,n,em); if (r) return r;
                n->p=sv; if (*pc<bl&&bc[*pc]==OP_EXIT_PROP) (*pc)++;
            } else { n->p=sv; *pc=skip; }
            break; }
        case OP_EXIT_PROP: return 0;
        case OP_NO_ADDITIONAL: {
            uint16_t ac=rd16(bc,pc);
            const char *ak[64]; int al[64];
            for (uint16_t i=0;i<ac&&i<64;i++) { al[i]=rd16(bc,pc); ak[i]=(const char*)(bc+*pc); *pc+=al[i]; }
            if (jtype(n)==JT_OBJ) { int sv=n->p; js_ws(n); n->p++;
                while (n->p<n->n&&n->s[n->p]!='}') { int kl; const char *k=js_str(n,&kl);
                    if (!k) break; bool found=false;
                    for (uint16_t i=0;i<ac;i++) if (kl==al[i]&&!memcmp(k,ak[i],kl)) { found=true; break; }
                    if (!found) { if(em) *em="additional properties not allowed"; n->p=sv; return R8E_SCHEMA_ERR_ADDITIONAL; }
                    js_ws(n); if (n->p<n->n&&n->s[n->p]==':') n->p++; js_skip(n);
                    js_ws(n); if (n->p<n->n&&n->s[n->p]==',') n->p++; }
                n->p=sv; }
            break; }
        case OP_ENTER_ARRAY: break;
        case OP_ITER_ITEMS: {
            uint16_t skip=rd16(bc,pc); int ipc=*pc;
            if (jtype(n)==JT_ARR) { int sv=n->p; js_ws(n); n->p++; js_ws(n);
                while (n->p<n->n&&n->s[n->p]!=']') {
                    int ip=ipc; int r=vrun(bc,bl,&ip,n,em);
                    if (r) { n->p=sv; return r; }
                    js_skip(n); js_ws(n);
                    if (n->p<n->n&&n->s[n->p]==',') n->p++; js_ws(n); }
                n->p=sv; }
            *pc=skip; if (*pc<bl&&bc[*pc]==OP_EXIT_ARRAY) (*pc)++;
            break; }
        case OP_EXIT_ARRAY: break;
        case OP_BRANCH_ANYOF: case OP_BRANCH_ONEOF: {
            bool oneof=(op==OP_BRANCH_ONEOF);
            uint16_t cnt=rd16(bc,pc); uint16_t bl_[32];
            for (uint16_t i=0;i<cnt&&i<32;i++) bl_[i]=rd16(bc,pc);
            int bp=*pc; bool any=false; int mc=0;
            for (uint16_t i=0;i<cnt&&i<32;i++) {
                int tp=bp, sv=n->p, be=bp+bl_[i];
                int r=vrun(bc,be,&tp,n,NULL); n->p=sv;
                if (!r) { any=true; mc++; }
                bp=be;
            }
            *pc=bp;
            if (!any) { if(em) *em=oneof?"no oneOf branch matched":"no anyOf branch matched";
                return R8E_SCHEMA_ERR_ONEOF; }
            if (oneof&&mc!=1) { if(em) *em="more than one oneOf branch matched";
                return R8E_SCHEMA_ERR_ONEOF; }
            break; }
        default: return 0;
        }
    }
    return 0;
}

/* --- Public API --- */

R8ESchemaValidator *r8e_schema_compile(const char *schema, int len) {
    if (!schema) return NULL;
    if (len<=0) len=(int)strlen(schema);
    R8ESchemaValidator *v = calloc(1, sizeof(*v));
    if (!v) return NULL;
    JS r = {schema, 0, len};
    compile(v, &r);
    em8(v, OP_DONE);
    if (!v->bc) { free(v); return NULL; }
    return v;
}

int r8e_schema_validate(const R8ESchemaValidator *v, const char *json,
                        int len, const char **error_msg) {
    if (!v||!v->bc) { if(error_msg) *error_msg="invalid validator"; return R8E_SCHEMA_ERR_COMPILE; }
    if (!json) { if(error_msg) *error_msg="null JSON input"; return R8E_SCHEMA_ERR_JSON; }
    if (len<=0) len=(int)strlen(json);
    JS n = {json, 0, len};
    int pc = 0;
    return vrun(v->bc, v->len, &pc, &n, error_msg);
}

void r8e_schema_free(R8ESchemaValidator *v) {
    if (!v) return;
    free(v->bc);
    free(v);
}
