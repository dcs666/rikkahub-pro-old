#include "rikka/json/json.h"
#include "rikka/core/buffer.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ================= 值解析 ================= */

typedef struct {
    Arena *arena;
    const char *p, *end;
    size_t err_pos;
    int depth;
} ParseCtx;

#define RJSON_MAX_DEPTH 512

static void skip_ws(ParseCtx *c) {
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r'))
        c->p++;
}

static RJson *new_node(ParseCtx *c, RJsonType t) {
    RJson *v = (RJson *)arena_alloc0(c->arena, 8, sizeof(RJson));
    if (!v) return NULL;
    v->type = t;
    return v;
}

static char *dup_str(ParseCtx *c, const char *s, size_t n) {
    char *d = (char *)arena_alloc(c->arena, 1, n + 1);
    if (!d) return NULL;
    memcpy(d, s, n);
    d[n] = '\0';
    return d;
}

/* 字符串字面量解析（含转义解码），返回解码后字节与长度（存 arena） */
static const char *parse_string(ParseCtx *c, size_t *out_len) {
    c->p++; /* 跳过 " */
    Buf raw;
    buf_init(&raw);
    int ok = 0;
    while (c->p < c->end) {
        unsigned char ch = (unsigned char)*c->p;
        if (ch == '"') { c->p++; ok = 1; break; }
        if (ch == '\\') {
            c->p++;
            if (c->p >= c->end) break;
            switch (*c->p) {
                case '"':  buf_append_byte(&raw, '"');  c->p++; break;
                case '\\': buf_append_byte(&raw, '\\'); c->p++; break;
                case '/':  buf_append_byte(&raw, '/');  c->p++; break;
                case 'b':  buf_append_byte(&raw, '\b'); c->p++; break;
                case 'f':  buf_append_byte(&raw, '\f'); c->p++; break;
                case 'n':  buf_append_byte(&raw, '\n'); c->p++; break;
                case 'r':  buf_append_byte(&raw, '\r'); c->p++; break;
                case 't':  buf_append_byte(&raw, '\t'); c->p++; break;
                case 'u': {
                    c->p++;
                    if (c->end - c->p < 4) goto done;
                    uint32_t cp = 0;
                    int valid = 1;
                    for (int i = 0; i < 4; i++) {
                        char h = c->p[i];
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (uint32_t)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (uint32_t)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (uint32_t)(h - 'A' + 10);
                        else { valid = 0; break; }
                    }
                    if (!valid) goto done;
                    c->p += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF) {
                        /* high surrogate：期待 \uDC00-\uDFFF */
                        if (c->end - c->p >= 6 && c->p[0] == '\\' && c->p[1] == 'u') {
                            uint32_t lo = 0;
                            int lv = 1;
                            for (int i = 0; i < 4; i++) {
                                char h = c->p[2 + i];
                                lo <<= 4;
                                if (h >= '0' && h <= '9') lo |= (uint32_t)(h - '0');
                                else if (h >= 'a' && h <= 'f') lo |= (uint32_t)(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') lo |= (uint32_t)(h - 'A' + 10);
                                else { lv = 0; break; }
                            }
                            if (lv && lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                c->p += 6;
                            }
                        }
                    }
                    char utf8[4]; size_t n8 = 0;
                    if (cp < 0x80) { utf8[0] = (char)cp; n8 = 1; }
                    else if (cp < 0x800) { utf8[0] = (char)(0xC0 | (cp >> 6)); utf8[1] = (char)(0x80 | (cp & 0x3F)); n8 = 2; }
                    else if (cp < 0x10000) { utf8[0] = (char)(0xE0 | (cp >> 12)); utf8[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); utf8[2] = (char)(0x80 | (cp & 0x3F)); n8 = 3; }
                    else { utf8[0] = (char)(0xF0 | (cp >> 18)); utf8[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); utf8[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); utf8[3] = (char)(0x80 | (cp & 0x3F)); n8 = 4; }
                    buf_append(&raw, utf8, n8);
                    break;
                }
                default: goto done;
            }
        } else {
            buf_append_byte(&raw, ch);
            c->p++;
        }
    }
done:
    if (!ok) {
        c->err_pos = (size_t)(c->p - c->p);
        buf_free(&raw);
        return NULL;
    }
    size_t n = raw.len;
    char *d = dup_str(c, (const char *)raw.data, n);
    buf_free(&raw);
    if (!d) return NULL;
    if (out_len) *out_len = n;
    return d;
}

static RJson *parse_value(ParseCtx *c);

static RJson *parse_object(ParseCtx *c) {
    if (++c->depth > RJSON_MAX_DEPTH) { c->err_pos = (size_t)(c->p - c->p); return NULL; }
    RJson *v = new_node(c, RJSON_OBJECT);
    if (!v) return NULL;
    c->p++; /* { */
    size_t cap = 8;
    v->u.obj.keys = (const char **)arena_alloc0(c->arena, 8, cap * sizeof(char *));
    v->u.obj.values = (RJson **)arena_alloc0(c->arena, 8, cap * sizeof(RJson *));
    if (!v->u.obj.keys || !v->u.obj.values) return NULL;
    skip_ws(c);
    if (c->p < c->end && *c->p == '}') { c->p++; return v; }
    for (;;) {
        skip_ws(c);
        if (c->p >= c->end || *c->p != '"') { c->err_pos = (size_t)(c->p - c->p); return NULL; }
        size_t klen = 0;
        const char *key = parse_string(c, &klen);
        if (!key) return NULL;
        skip_ws(c);
        if (c->p >= c->end || *c->p != ':') { c->err_pos = (size_t)(c->p - c->p); c->depth--; return NULL; }
        c->p++;
        skip_ws(c);
        RJson *val = parse_value(c);
        if (!val) { c->depth--; return NULL; }
        if (v->u.obj.count == cap) {
            if (cap > SIZE_MAX / 2 / sizeof(RJson *)) return NULL; /* 溢出防护 */
            cap *= 2;
            const char **nk = (const char **)arena_alloc0(c->arena, 8, cap * sizeof(char *));
            RJson **nv = (RJson **)arena_alloc0(c->arena, 8, cap * sizeof(RJson *));
            if (!nk || !nv) return NULL;
            memcpy(nk, v->u.obj.keys, v->u.obj.count * sizeof(char *));
            memcpy(nv, v->u.obj.values, v->u.obj.count * sizeof(RJson *));
            v->u.obj.keys = nk; v->u.obj.values = nv;
        }
        v->u.obj.keys[v->u.obj.count] = key;
        v->u.obj.values[v->u.obj.count] = val;
        v->u.obj.count++;
        skip_ws(c);
        if (c->p >= c->end) { c->err_pos = (size_t)(c->p - c->p); return NULL; }
        if (*c->p == ',') { c->p++; continue; }
        if (*c->p == '}') { c->p++; c->depth--; return v; }
        c->err_pos = (size_t)(c->p - c->p);
        c->depth--;
        return NULL;
    }
}

static RJson *parse_array(ParseCtx *c) {
    if (++c->depth > RJSON_MAX_DEPTH) { c->err_pos = (size_t)(c->p - c->p); return NULL; }
    RJson *v = new_node(c, RJSON_ARRAY);
    if (!v) return NULL;
    c->p++; /* [ */
    size_t cap = 8;
    v->u.arr.items = (RJson **)arena_alloc0(c->arena, 8, cap * sizeof(RJson *));
    if (!v->u.arr.items) return NULL;
    skip_ws(c);
    if (c->p < c->end && *c->p == ']') { c->p++; c->depth--; return v; }
    for (;;) {
        skip_ws(c);
        RJson *val = parse_value(c);
        if (!val) { c->depth--; return NULL; }
        if (v->u.arr.count == cap) {
            if (cap > SIZE_MAX / 2 / sizeof(RJson *)) return NULL; /* 溢出防护 */
            cap *= 2;
            RJson **ni = (RJson **)arena_alloc0(c->arena, 8, cap * sizeof(RJson *));
            if (!ni) return NULL;
            memcpy(ni, v->u.arr.items, v->u.arr.count * sizeof(RJson *));
            v->u.arr.items = ni;
        }
        v->u.arr.items[v->u.arr.count++] = val;
        skip_ws(c);
        if (c->p >= c->end) { c->err_pos = (size_t)(c->p - c->p); c->depth--; return NULL; }
        if (*c->p == ',') { c->p++; continue; }
        if (*c->p == ']') { c->p++; c->depth--; return v; }
        c->err_pos = (size_t)(c->p - c->p);
        c->depth--;
        return NULL;
    }
}

static RJson *parse_number(ParseCtx *c) {
    const char *start = c->p;
    if (c->p < c->end && *c->p == '-') c->p++;
    while (c->p < c->end && (*c->p >= '0' && *c->p <= '9')) c->p++;
    if (c->p < c->end && *c->p == '.') {
        c->p++;
        while (c->p < c->end && (*c->p >= '0' && *c->p <= '9')) c->p++;
    }
    if (c->p < c->end && (*c->p == 'e' || *c->p == 'E')) {
        c->p++;
        if (c->p < c->end && (*c->p == '+' || *c->p == '-')) c->p++;
        while (c->p < c->end && (*c->p >= '0' && *c->p <= '9')) c->p++;
    }
    if (c->p == start) { c->err_pos = (size_t)(c->p - start); return NULL; }
    char tmp[64];
    size_t n = (size_t)(c->p - start);
    if (n >= sizeof(tmp)) { c->err_pos = (size_t)(c->p - start); return NULL; }
    memcpy(tmp, start, n);
    tmp[n] = '\0';
    RJson *v = new_node(c, RJSON_NUMBER);
    if (!v) return NULL;
    v->u.number = strtod(tmp, NULL);
    return v;
}

static RJson *parse_literal(ParseCtx *c, const char *lit, RJsonType t, int bval) {
    size_t n = strlen(lit);
    if ((size_t)(c->end - c->p) < n || memcmp(c->p, lit, n) != 0) {
        c->err_pos = (size_t)(c->p - c->p);
        return NULL;
    }
    c->p += n;
    RJson *v = new_node(c, t);
    if (!v) return NULL;
    if (t == RJSON_BOOL) v->u.boolean = bval;
    return v;
}

static RJson *parse_value(ParseCtx *c) {
    skip_ws(c);
    if (c->p >= c->end) { c->err_pos = (size_t)(c->p - c->p); return NULL; }
    switch (*c->p) {
        case '{': return parse_object(c);
        case '[': return parse_array(c);
        case '"': {
            size_t len = 0;
            const char *s = parse_string(c, &len);
            if (!s) return NULL;
            RJson *v = new_node(c, RJSON_STRING);
            if (!v) return NULL;
            v->u.str.ptr = s;
            v->u.str.len = len;
            return v;
        }
        case 't': return parse_literal(c, "true", RJSON_BOOL, 1);
        case 'f': return parse_literal(c, "false", RJSON_BOOL, 0);
        case 'n': return parse_literal(c, "null", RJSON_NULL, 0);
        default: return parse_number(c);
    }
}

RJson *rjson_parse(Arena *arena, const char *text, size_t len, size_t *err_pos) {
    ParseCtx c;
    c.arena = arena;
    /* 跳过 UTF-8 BOM(EF BB BF) — 部分服务器响应带 BOM */
    if (len >= 3 && (unsigned char)text[0] == 0xEF &&
        (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) {
        text += 3;
        len -= 3;
    }
    c.p = text;
    c.end = text + len;
    c.err_pos = 0;
    c.depth = 0;
    RJson *v = parse_value(&c);
    if (!v) {
        if (err_pos) *err_pos = c.err_pos;
        return NULL;
    }
    skip_ws(&c);
    if (c.p != c.end) {
        if (err_pos) *err_pos = (size_t)(c.p - text);
        return NULL;
    }
    return v;
}

const RJson *rjson_obj_get(const RJson *obj, const char *key) {
    if (!obj || obj->type != RJSON_OBJECT) return NULL;
    for (size_t i = 0; i < obj->u.obj.count; i++) {
        if (strcmp(obj->u.obj.keys[i], key) == 0) return obj->u.obj.values[i];
    }
    return NULL;
}

const RJson *rjson_arr_at(const RJson *arr, size_t idx) {
    if (!arr || arr->type != RJSON_ARRAY || idx >= arr->u.arr.count) return NULL;
    return arr->u.arr.items[idx];
}

int rjson_is(const RJson *v, RJsonType t) { return v && v->type == t; }

const char *rjson_str(const RJson *v, size_t *len) {
    if (!v || v->type != RJSON_STRING) return NULL;
    if (len) *len = v->u.str.len;
    return v->u.str.ptr;
}

/* ================= 序列化 ================= */

void rjson_out_init(RJsonOut *o) { o->buf = NULL; o->len = 0; o->cap = 0; }
void rjson_out_free(RJsonOut *o) { free(o->buf); o->buf = NULL; o->len = o->cap = 0; }

static void out_reserve(RJsonOut *o, size_t extra) {
    size_t need = o->len + extra;
    if (need <= o->cap) return;
    size_t nc = o->cap ? o->cap : 64;
    while (nc < need) nc *= 2;
    char *nb = (char *)realloc(o->buf, nc);
    if (!nb) return;
    o->buf = nb;
    o->cap = nc;
}

static void out_append(RJsonOut *o, const char *s, size_t n) {
    out_reserve(o, n + 1);           /* +1 保证 NUL 结尾 */
    memcpy(o->buf + o->len, s, n);
    o->len += n;
    o->buf[o->len] = '\0';           /* 不变式：buf 始终是合法 C 字符串 */
}

static void hex4(RJsonOut *o, uint32_t cp) {
    static const char hex[] = "0123456789abcdef";
    char tmp[6] = { '\\', 'u', 0, 0, 0, 0 };
    tmp[2] = hex[(cp >> 12) & 0xF];
    tmp[3] = hex[(cp >> 8) & 0xF];
    tmp[4] = hex[(cp >> 4) & 0xF];
    tmp[5] = hex[cp & 0xF];
    out_append(o, tmp, 6);
}

void rjson_write_escaped(RJsonOut *o, const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)s[i];
        switch (ch) {
            case '"':  out_append(o, "\\\"", 2); break;
            case '\\': out_append(o, "\\\\", 2); break;
            case '\b': out_append(o, "\\b", 2); break;
            case '\f': out_append(o, "\\f", 2); break;
            case '\n': out_append(o, "\\n", 2); break;
            case '\r': out_append(o, "\\r", 2); break;
            case '\t': out_append(o, "\\t", 2); break;
            default:
                if (ch < 0x20) hex4(o, ch);
                else out_append(o, (const char *)&ch, 1);
        }
    }
}

void rjson_write_string(RJsonOut *o, const char *s, size_t len) {
    out_append(o, "\"", 1);
    rjson_write_escaped(o, s, len);
    out_append(o, "\"", 1);
}

void rjson_write_value(RJsonOut *o, const RJson *v) {
    if (!v) { out_append(o, "null", 4); return; }
    switch (v->type) {
        case RJSON_NULL:   out_append(o, "null", 4); break;
        case RJSON_BOOL:   out_append(o, v->u.boolean ? "true" : "false", v->u.boolean ? 4 : 5); break;
        case RJSON_NUMBER: {
            char tmp[64];
            int n = snprintf(tmp, sizeof(tmp), "%.17g", v->u.number);
            if (n < 0) n = 0;
            out_append(o, tmp, (size_t)n);
            break;
        }
        case RJSON_STRING: rjson_write_string(o, v->u.str.ptr, v->u.str.len); break;
        case RJSON_ARRAY: {
            out_append(o, "[", 1);
            for (size_t i = 0; i < v->u.arr.count; i++) {
                if (i) out_append(o, ",", 1);
                rjson_write_value(o, v->u.arr.items[i]);
            }
            out_append(o, "]", 1);
            break;
        }
        case RJSON_OBJECT: {
            out_append(o, "{", 1);
            for (size_t i = 0; i < v->u.obj.count; i++) {
                if (i) out_append(o, ",", 1);
                rjson_write_string(o, v->u.obj.keys[i], strlen(v->u.obj.keys[i]));
                out_append(o, ":", 1);
                rjson_write_value(o, v->u.obj.values[i]);
            }
            out_append(o, "}", 1);
            break;
        }
    }
}

/* ================= 增量流式提取 ================= */

#define RJSON_STREAM_MAX_DEPTH 64
#define RJSON_STREAM_KEY_MAX   255

typedef struct {
    int  kind;          /* 0 = object, 1 = array */
    size_t index;       /* array: 下一个元素索引 */
    char key[RJSON_STREAM_KEY_MAX + 1];
    size_t key_len;
    int  key_ready;
    int  on_path_len;   /* -1 = 不在目标路径；否则=已匹配前缀长度（=该层深度） */
} SFrame;

struct RJsonStream {
    const RJsonStreamPathElem *path;
    size_t path_len;
    RJsonStreamSink sink;
    void *sink_ctx;

    SFrame stack[RJSON_STREAM_MAX_DEPTH];
    int depth;
    int state;

    int pending_match;   /* 值开始前计算的匹配：-1 未定；0 不匹配；>0 前缀长度 */
    int hit;
    int done;
    int error;

    /* 捕获模式 */
    int cap_raw_depth;
    uint32_t cap_hi;
    int cap_u_need;      /* 还缺的 hex 位数 */
    uint32_t cap_u_val;

    /* 跳过模式辅助 */
    int lit_pos;
    const char *lit;
    int skip_u_need;
    int skip_key_mode;   /* 当前转义序列是否处于对象键中 */

    int bom_skip;        /* 根值前 UTF-8 BOM 跳过进度(0-3) */
};

/* 内部状态 */
enum {
    ST_ROOT = 0,        /* 期待根值 */
    ST_VALUE,           /* 值即将开始（'[' 后或 ':' 后或 ',' 后） */
    ST_OBJ_KEY_OR_END,  /* 对象：期待键或 '}' */
    ST_OBJ_COLON,       /* 对象：键后期待 ':' */
    ST_OBJ_COMMA_OR_END,/* 对象：值后期待 ',' 或 '}' */
    ST_ARR_ELEM_OR_END, /* 数组：期待值或 ']' */
    ST_ARR_COMMA_OR_END,/* 数组：值后期待 ',' 或 ']' */
    ST_STR,             /* 字符串体（跳过模式，可能收集键） */
    ST_STR_ESC,         /* 跳过模式：转义字符 */
    ST_STR_ESC_U,       /* 跳过模式：\u 后数 hex（跳过） */
    ST_CAP_STR,         /* 捕获：字符串体（解码输出） */
    ST_CAP_STR_ESC,     /* 捕获：转义字符 */
    ST_CAP_STR_U,       /* 捕获：\u 收集 hex */
    ST_CAP_STR_U_HI,    /* 捕获：high surrogate 后期待 \u */
    ST_CAP_STR_U_HI_BS, /* 捕获：期待 'u' */
    ST_CAP_STR_U_LO,    /* 捕获：low surrogate hex */
    ST_CAP_RAW,         /* 捕获：原始子树 */
    ST_CAP_RAW_STR,     /* 捕获原始子树内的字符串 */
    ST_CAP_RAW_STR_ESC, /* 捕获原始子树内字符串的转义 */
    ST_NUM,             /* 数字 */
    ST_LIT,             /* 字面量 */
    ST_AFTER,           /* 根值结束 */
};

/* 值结束后进入的状态：容器内等待逗号/右括号，根则收尾 */
static SFrame *top(RJsonStream *s) { return &s->stack[s->depth - 1]; }

static int after_value_state(RJsonStream *s) {
    if (s->depth == 0) return ST_AFTER;
    return top(s)->kind == 0 ? ST_OBJ_COMMA_OR_END : ST_ARR_COMMA_OR_END;
}

static void sink_out(RJsonStream *s, const char *data, size_t len) {
    if (len && s->sink) s->sink(s->sink_ctx, data, len);
}

static void utf8_out(RJsonStream *s, uint32_t cp) {
    char tmp[4]; size_t n;
    if (cp < 0x80) { tmp[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { tmp[0] = (char)(0xC0 | (cp >> 6)); tmp[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { tmp[0] = (char)(0xE0 | (cp >> 12)); tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else { tmp[0] = (char)(0xF0 | (cp >> 18)); tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    sink_out(s, tmp, n);
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int in_key(RJsonStream *s) {
    return s->depth > 0 && top(s)->kind == 0 && !top(s)->key_ready;
}

static void key_append(RJsonStream *s, char ch) {
    if (in_key(s) && top(s)->key_len < RJSON_STREAM_KEY_MAX) {
        top(s)->key[top(s)->key_len++] = ch;
    }
}

/* 值开始前：根据父帧与 path[depth-1] 计算匹配前缀长度（0=不匹配） */
static void compute_match(RJsonStream *s) {
    int d = s->depth;
    s->pending_match = 0;
    if (d == 0) return; /* 根不匹配任何非空路径 */
    SFrame *f = top(s);
    if (f->on_path_len != d - 1) { s->pending_match = 0; return; } /* 父不在路径上 */
    if (d > (int)s->path_len) { s->pending_match = 0; return; }
    const RJsonStreamPathElem *e = &s->path[d - 1];
    int m;
    if (f->kind == 1) { /* array */
        m = e->is_index && e->u.index == f->index;
    } else {
        m = (!e->is_index) && f->key_ready && strcmp(f->key, e->u.key) == 0;
    }
    s->pending_match = m ? d : 0;
}

/* 值开始：取匹配深度并复位。命中 = pending == path_len */
static int value_match(RJsonStream *s) {
    if (s->pending_match < 0) compute_match(s);
    int m = s->pending_match;
    s->pending_match = -1;
    return m;
}

static int push_frame(RJsonStream *s, int kind, int match_len) {
    if (s->depth >= RJSON_STREAM_MAX_DEPTH) return 0;
    SFrame *f = &s->stack[s->depth++];
    memset(f, 0, sizeof(SFrame));
    f->kind = kind;
    f->on_path_len = match_len;
    return 1;
}

static void pop_frame(RJsonStream *s) {
    if (s->depth > 0) s->depth--;
}

RJsonStream *rjson_stream_create(const RJsonStreamPathElem *path,
                                 RJsonStreamSink sink, void *sink_ctx) {
    RJsonStream *s = (RJsonStream *)calloc(1, sizeof(RJsonStream));
    if (!s) return NULL;
    s->path = path;
    s->sink = sink;
    s->sink_ctx = sink_ctx;
    s->path_len = 0;
    if (path) while (path[s->path_len].is_index != -2) s->path_len++;
    s->state = ST_ROOT;
    s->pending_match = -1;
    s->bom_skip = 0;
    return s;
}

void rjson_stream_destroy(RJsonStream *s) {
    if (s) free(s);
}

void rjson_stream_set_path(RJsonStream *s, const RJsonStreamPathElem *path) {
    s->path = path;
    s->path_len = 0;
    if (path) while (path[s->path_len].is_index != -2) s->path_len++;
}

void rjson_stream_reset(RJsonStream *s) {
    s->depth = 0;
    s->state = ST_ROOT;
    s->pending_match = -1;
    s->bom_skip = 0;
    s->hit = 0;
    s->done = 0;
    s->error = 0;
    s->cap_raw_depth = 0;
    s->cap_hi = 0;
    s->cap_u_need = 0;
    s->cap_u_val = 0;
    s->lit_pos = 0;
    s->skip_u_need = 0;
    s->skip_key_mode = 0;
}

int rjson_stream_hit(const RJsonStream *s) { return s->hit; }

RJsonStreamStatus rjson_stream_feed(RJsonStream *s, const char *data, size_t len) {
    if (s->error) return RJSON_STREAM_ERROR;
    if (s->done) return RJSON_STREAM_DONE;
    size_t i = 0;
    while (i < len && !s->error && !s->done) {
        char ch = data[i++];
        switch (s->state) {
        case ST_ROOT:
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') break;
            /* UTF-8 BOM(EF BB BF) 跳过 — 根值前的 3 字节 */
            if (s->bom_skip < 3) {
                static const unsigned char bom[3] = {0xEF, 0xBB, 0xBF};
                if ((unsigned char)ch == bom[s->bom_skip]) { s->bom_skip++; break; }
                s->bom_skip = 3; /* 非 BOM 开头, 不再跳过 */
            }
            s->state = ST_VALUE;
            i--; /* 交给 ST_VALUE */
            break;

        case ST_VALUE: {
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') break;
            int m = value_match(s);
                if (ch == '{' || ch == '[') {
                if (m == (int)s->path_len && s->path_len > 0) {
                    /* 目标是容器：捕获整棵子树原始字节 */
                    s->cap_raw_depth = 1;
                    s->hit = 1;
                    sink_out(s, &ch, 1);
                    s->state = ST_CAP_RAW;
                } else {
                    if (!push_frame(s, ch == '{' ? 0 : 1, m)) { s->error = 1; break; }
                    s->state = ch == '{' ? ST_OBJ_KEY_OR_END : ST_ARR_ELEM_OR_END;
                }
            } else if (ch == '"') {
                if (m == (int)s->path_len && s->path_len > 0) {
                    s->hit = 1;
                    s->state = ST_CAP_STR;
                } else {
                    s->state = ST_STR;
                }
            } else if (ch == 't' || ch == 'f' || ch == 'n') {
                s->lit = (ch == 't') ? "true" : (ch == 'f') ? "false" : "null";
                s->lit_pos = 1;
                s->state = ST_LIT;
            } else if (ch == '-' || (ch >= '0' && ch <= '9')) {
                s->state = ST_NUM;
            } else {
                s->error = 1;
            }
            break;
        }

        case ST_OBJ_KEY_OR_END:
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') break;
            if (ch == '}') { pop_frame(s); s->state = after_value_state(s); break; }
            if (ch == '"') {
                SFrame *f = top(s);
                f->key_len = 0;
                f->key_ready = 0;
                s->state = ST_STR;
            } else { s->error = 1; }
            break;

        case ST_OBJ_COLON:
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') break;
            if (ch == ':') s->state = ST_VALUE;
            else s->error = 1;
            break;

        case ST_OBJ_COMMA_OR_END:
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') break;
            if (ch == ',') s->state = ST_OBJ_KEY_OR_END;
            else if (ch == '}') { pop_frame(s); s->state = after_value_state(s); }
            else s->error = 1;
            break;

        case ST_ARR_ELEM_OR_END:
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') break;
            if (ch == ']') { pop_frame(s); s->state = after_value_state(s); break; }
            s->state = ST_VALUE;
            i--; /* 回退处理值 */
            break;

        case ST_ARR_COMMA_OR_END:
            if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') break;
            if (ch == ',') { top(s)->index++; s->state = ST_ARR_ELEM_OR_END; }
            else if (ch == ']') { pop_frame(s); s->state = after_value_state(s); }
            else s->error = 1;
            break;

        case ST_STR: {
            /* 快速路径：批量跳过/收集（含 ch）到下一个 " 或 \（仅当 ch 为普通字符） */
            if (ch != '"' && ch != '\\') {
                const char *start = data + i - 1; /* 包含已消费的 ch */
                const char *rest = data + i;
                size_t rl = len - i;
                const char *q = memchr(rest, '"', rl);
                const char *bs = memchr(rest, '\\', rl);
                const char *stop = (q && bs) ? (q < bs ? q : bs) : (q ? q : bs);
                size_t total = stop ? (size_t)(stop - start) : (rl + 1);
                if (in_key(s)) {
                    size_t room = RJSON_STREAM_KEY_MAX - top(s)->key_len;
                    if (room == 0) {
                        /* 键超长：不再收集，标记结束（匹配必然失败，正常跳过） */
                        top(s)->key[top(s)->key_len] = '\0';
                        top(s)->key_ready = 1;
                    } else {
                        if (total > room) total = room;
                        memcpy(top(s)->key + top(s)->key_len, start, total);
                        top(s)->key_len += total;
                    }
                }
                i += total - 1;
                if (stop && total == (size_t)(stop - start)) break; /* 下一轮处理 stop */
                i = len; /* 剩余全部消费 */
                break;
            }
            if (ch == '"') {
                if (in_key(s)) {
                    top(s)->key[top(s)->key_len] = '\0';
                    top(s)->key_ready = 1;
                    s->state = ST_OBJ_COLON;
                } else {
                    s->state = after_value_state(s);
                }
            } else if (ch == '\\') {
                s->skip_key_mode = in_key(s);
                s->state = ST_STR_ESC;
            } else {
                key_append(s, ch);
            }
            break;
        }

        case ST_STR_ESC: {
            char out;
            switch (ch) {
                case '"':  out = '"';  s->state = ST_STR; break;
                case '\\': out = '\\'; s->state = ST_STR; break;
                case '/':  out = '/';  s->state = ST_STR; break;
                case 'b':  out = '\b'; s->state = ST_STR; break;
                case 'f':  out = '\f'; s->state = ST_STR; break;
                case 'n':  out = '\n'; s->state = ST_STR; break;
                case 'r':  out = '\r'; s->state = ST_STR; break;
                case 't':  out = '\t'; s->state = ST_STR; break;
                case 'u':  s->skip_u_need = 4; s->state = ST_STR_ESC_U; continue;
                default: s->error = 1; continue;
            }
            key_append(s, out);
            break;
        }

        case ST_STR_ESC_U:
            /* 跳过模式：数 4 个 hex 后回到 ST_STR。键中的 \u 转义不解码（限制，见 header） */
            if (hex_val(ch) < 0) { s->error = 1; break; }
            if (--s->skip_u_need == 0) s->state = ST_STR;
            break;

        case ST_CAP_STR: {
            /* 快速路径：整段批量输出（含 ch）直到 " 或 \（仅当 ch 为普通字符） */
            if (ch != '"' && ch != '\\') {
                const char *start = data + i - 1; /* 包含已消费的 ch */
                const char *rest = data + i;
                size_t rl = len - i;
                const char *q = memchr(rest, '"', rl);
                const char *bs = memchr(rest, '\\', rl);
                const char *stop = (q && bs) ? (q < bs ? q : bs) : (q ? q : bs);
                if (stop) {
                    size_t n = (size_t)(stop - start); /* 至少 1（含 ch） */
                    sink_out(s, start, n);
                    i += n - 1; /* 下一轮 ch = data[i++] 取 stop */
                } else {
                    sink_out(s, start, rl + 1); /* 剩余全部 */
                    i = len;
                }
                break;
            }
            if (ch == '"') {
                s->state = after_value_state(s);
            } else if (ch == '\\') {
                s->state = ST_CAP_STR_ESC;
            } else {
                sink_out(s, &ch, 1);
            }
            break;
        }

        case ST_CAP_STR_ESC:
            switch (ch) {
                case '"':  sink_out(s, "\"", 1); s->state = ST_CAP_STR; break;
                case '\\': sink_out(s, "\\", 1); s->state = ST_CAP_STR; break;
                case '/':  sink_out(s, "/", 1);  s->state = ST_CAP_STR; break;
                case 'b':  sink_out(s, "\b", 1); s->state = ST_CAP_STR; break;
                case 'f':  sink_out(s, "\f", 1); s->state = ST_CAP_STR; break;
                case 'n':  sink_out(s, "\n", 1); s->state = ST_CAP_STR; break;
                case 'r':  sink_out(s, "\r", 1); s->state = ST_CAP_STR; break;
                case 't':  sink_out(s, "\t", 1); s->state = ST_CAP_STR; break;
                case 'u':  s->cap_u_val = 0; s->cap_u_need = 4; s->state = ST_CAP_STR_U; break;
                default: s->error = 1; break;
            }
            break;

        case ST_CAP_STR_U: {
            int hv = hex_val(ch);
            if (hv < 0) { s->error = 1; break; }
            s->cap_u_val = (s->cap_u_val << 4) | (uint32_t)hv;
            if (--s->cap_u_need == 0) {
                uint32_t cp = s->cap_u_val;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    s->cap_hi = cp;
                    s->state = ST_CAP_STR_U_HI;
                } else {
                    utf8_out(s, cp);
                    s->state = ST_CAP_STR;
                }
            }
            break;
        }

        case ST_CAP_STR_U_HI:
            if (ch == '\\') {
                s->state = ST_CAP_STR_U_HI_BS;
            } else {
                /* 孤立 high surrogate（非法但对真实 API 输出宽容）：替换符 + 回退处理 ch */
                utf8_out(s, 0xFFFD);
                s->state = ST_CAP_STR;
                i--;
            }
            break;

        case ST_CAP_STR_U_HI_BS:
            if (ch == 'u') { s->cap_u_val = 0; s->cap_u_need = 4; s->state = ST_CAP_STR_U_LO; }
            else s->error = 1;
            break;

        case ST_CAP_STR_U_LO: {
            int hv = hex_val(ch);
            if (hv < 0) { s->error = 1; break; }
            s->cap_u_val = (s->cap_u_val << 4) | (uint32_t)hv;
            if (--s->cap_u_need == 0) {
                uint32_t lo = s->cap_u_val;
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    uint32_t cp = 0x10000 + ((s->cap_hi - 0xD800) << 10) + (lo - 0xDC00);
                    utf8_out(s, cp);
                } else {
                    utf8_out(s, 0xFFFD); /* 孤立 high surrogate */
                }
                s->state = ST_CAP_STR;
            }
            break;
        }

        case ST_CAP_RAW: {
            /* 快速路径：批量输出（含 ch）到结构字符 { } [ ] " \（仅当 ch 为普通字符） */
            if (ch != '{' && ch != '}' && ch != '[' && ch != ']' && ch != '"' && ch != '\\') {
                const char *start = data + i - 1; /* 包含已消费的 ch */
                const char *rest = data + i;
                size_t rl = len - i;
                const char *stop = NULL;
                for (size_t j = 0; j < rl; j++) {
                    char c = rest[j];
                    if (c == '{' || c == '}' || c == '[' || c == ']' || c == '"' || c == '\\') {
                        stop = rest + j;
                        break;
                    }
                }
                if (stop) {
                    size_t n = (size_t)(stop - start);
                    sink_out(s, start, n);
                    i += n - 1; /* 下一轮 ch = data[i++] 取 stop */
                } else {
                    sink_out(s, start, rl + 1);
                    i = len;
                }
                break;
            }
            if (ch == '{' || ch == '[') { s->cap_raw_depth++; sink_out(s, &ch, 1); }
            else if (ch == '}' || ch == ']') {
                s->cap_raw_depth--;
                sink_out(s, &ch, 1);
                if (s->cap_raw_depth <= 0) s->state = after_value_state(s);
            } else if (ch == '"') {
                sink_out(s, &ch, 1);
                s->state = ST_CAP_RAW_STR;
            } else {
                sink_out(s, &ch, 1);
            }
            break;
        }

        case ST_CAP_RAW_STR:
            sink_out(s, &ch, 1);
            if (ch == '\\') s->state = ST_CAP_RAW_STR_ESC;
            else if (ch == '"') s->state = ST_CAP_RAW;
            break;

        case ST_CAP_RAW_STR_ESC:
            sink_out(s, &ch, 1);
            s->state = ST_CAP_RAW_STR;
            break;

        case ST_NUM: {
            /* 快速路径：批量消费数字字符 */
            const char *rest = data + i;
            size_t rl = len - i;
            size_t j = 0;
            while (j < rl) {
                char c = rest[j];
                if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E')
                    j++;
                else
                    break;
            }
            if (j > 0) { i += j; break; } /* 剩余分隔符交给下一轮判定 */
            if ((ch >= '0' && ch <= '9') || ch == '-' || ch == '+' || ch == '.' || ch == 'e' || ch == 'E') {
                /* 继续数字 */
            } else {
                s->state = after_value_state(s);
                i--; /* 回退处理分隔符 */
            }
            break;
        }

        case ST_LIT:
            if (s->lit && s->lit[s->lit_pos] == ch) {
                s->lit_pos++;
                if (s->lit[s->lit_pos] == '\0') s->state = after_value_state(s);
            } else {
                s->error = 1;
            }
            break;

        case ST_AFTER:
            s->done = 1;
            break;

        default:
            s->error = 1;
            break;
        }
    }
    if (s->error) return RJSON_STREAM_ERROR;
    if (s->done) return RJSON_STREAM_DONE;
    return RJSON_STREAM_OK;
}

RJsonStreamStatus rjson_stream_finish(RJsonStream *s) {
    if (s->error) return RJSON_STREAM_ERROR;
    if (s->done) return RJSON_STREAM_DONE;
    if (s->state == ST_AFTER || s->state == ST_ROOT) {
        s->done = 1;
        return RJSON_STREAM_DONE;
    }
    s->error = 1;
    return RJSON_STREAM_ERROR;
}
