#include "rikka/data/index.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static size_t utf8_len(unsigned char b);

/* ---------- 分词 ---------- */

size_t rk_tokenize(const char *text, size_t len, RkTokenCb cb, void *ctx) {
    size_t count = 0;
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 0x80) {
            /* 中文：字符级 bigram（按 UTF-8 字符边界，重叠滑动） */
            size_t j = i;
            size_t chars = 0;
            while (j < len && (unsigned char)text[j] >= 0x80) {
                unsigned char b = (unsigned char)text[j];
                size_t clen = (b >= 0xF0) ? 4 : (b >= 0xE0) ? 3 : (b >= 0xC0) ? 2 : 1;
                if (j + clen > len) break;
                j += clen;
                chars++;
            }
            if (chars >= 2) {
                size_t k = i;
                while (k + utf8_len(text[k]) < j) {
                    size_t c1 = utf8_len(text[k]);
                    size_t c2 = utf8_len(text[k + c1]);
                    if (cb) cb(ctx, text + k, c1 + c2);
                    count++;
                    k += c1;
                }
                i = j;
                continue;
            }
            i = j > i ? j : i + 1;
            continue;
        }
        if (isalnum(c) || c == '_') {
            size_t j = i;
            while (j < len && (isalnum((unsigned char)text[j]) || text[j] == '_')) j++;
            if (cb) cb(ctx, text + i, j - i);
            count++;
            i = j;
            continue;
        }
        i++;
    }
    return count;
}

/* ---------- 倒排索引 ---------- */

typedef struct Posting {
    uint64_t *docs;
    size_t count, cap;
} Posting;

typedef struct TokNode {
    char *tok;
    size_t tlen;
    uint64_t hash;
    Posting post;
    struct TokNode *hnext;
} TokNode;

struct RkIndex {
    TokNode **buckets;
    size_t nbuckets;
    size_t tok_count;
    size_t doc_count;
};

static size_t utf8_len(unsigned char b) {
    if (b >= 0xF0) return 4;
    if (b >= 0xE0) return 3;
    if (b >= 0xC0) return 2;
    return 1;
}

static uint64_t fnv1a(const void *key, size_t len) {
    const uint8_t *p = (const uint8_t *)key;
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

RkIndex *rk_index_create(void) {
    RkIndex *ix = (RkIndex *)calloc(1, sizeof(RkIndex));
    if (!ix) return NULL;
    ix->nbuckets = 4096;
    ix->buckets = (TokNode **)calloc(ix->nbuckets, sizeof(TokNode *));
    if (!ix->buckets) { free(ix); return NULL; }
    return ix;
}

void rk_index_destroy(RkIndex *ix) {
    if (!ix) return;
    for (size_t i = 0; i < ix->nbuckets; i++) {
        TokNode *n = ix->buckets[i];
        while (n) {
            TokNode *nx = n->hnext;
            free(n->tok);
            free(n->post.docs);
            free(n);
            n = nx;
        }
    }
    free(ix->buckets);
    free(ix);
}

static TokNode *find_node(RkIndex *ix, const char *tok, size_t tlen, uint64_t h) {
    size_t bi = h & (ix->nbuckets - 1);
    for (TokNode *n = ix->buckets[bi]; n; n = n->hnext)
        if (n->hash == h && n->tlen == tlen && memcmp(n->tok, tok, tlen) == 0)
            return n;
    return NULL;
}

static void posting_add(Posting *p, uint64_t doc) {
    if (p->count > 0 && p->docs[p->count - 1] == doc) return;
    if (p->count == p->cap) {
        size_t nc = p->cap ? p->cap * 2 : 8;
        uint64_t *nd = (uint64_t *)realloc(p->docs, nc * sizeof(uint64_t));
        if (!nd) return;
        p->docs = nd;
        p->cap = nc;
    }
    p->docs[p->count++] = doc;
}

/* 二分：doc 是否在 posting（docs 有序） */
static int posting_contains(const Posting *p, uint64_t doc) {
    size_t lo = 0, hi = p->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (p->docs[mid] == doc) return 1;
        if (p->docs[mid] < doc) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

typedef struct {
    RkIndex *ix;
    uint64_t doc;
} AddCtx;

static void tok_cb(void *vctx, const char *tok, size_t tlen) {
    AddCtx *c = (AddCtx *)vctx;
    RkIndex *ix = c->ix;
    uint64_t h = fnv1a(tok, tlen);
    TokNode *n = find_node(ix, tok, tlen, h);
    if (!n) {
        size_t bi = h & (ix->nbuckets - 1);
        n = (TokNode *)calloc(1, sizeof(TokNode));
        if (!n) return;
        n->tok = (char *)malloc(tlen ? tlen : 1);
        if (!n->tok) { free(n); return; }
        memcpy(n->tok, tok, tlen);
        n->tlen = tlen;
        n->hash = h;
        n->hnext = ix->buckets[bi];
        ix->buckets[bi] = n;
        ix->tok_count++;
    }
    posting_add(&n->post, c->doc);
}

size_t rk_index_add(RkIndex *ix, uint64_t doc, const char *text, size_t len) {
    size_t before = ix->tok_count;
    AddCtx ctx = {ix, doc};
    rk_tokenize(text, len, tok_cb, &ctx);
    ix->doc_count++;
    return ix->tok_count - before;
}

typedef struct {
    RkIndex *ix;
    const TokNode *nodes[64];
    size_t ntok;
    int missing; /* 存在未索引 token：AND 结果必空 */
} QCtx;

static void q_cb(void *vctx, const char *tok, size_t tlen) {
    QCtx *q = (QCtx *)vctx;
    uint64_t h = fnv1a(tok, tlen);
    TokNode *n = find_node(q->ix, tok, tlen, h);
    if (!n) { q->missing = 1; return; } /* 未索引 token：结果必空 */
    if (q->ntok < 64) q->nodes[q->ntok] = n;
    q->ntok++;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

size_t rk_index_search(RkIndex *ix, const char *query, size_t len,
                       uint64_t *out, size_t cap) {
    if (cap == 0) return 0;
    QCtx q;
    q.ix = ix;
    q.ntok = 0;
    q.missing = 0;
    rk_tokenize(query, len, q_cb, &q);
    if (q.missing) return 0; /* 有未索引 token：无文档可命中 */
    if (q.ntok == 0 || q.ntok > 64) return 0;
    /* 选 posting 最短的做候选集 */
    const TokNode *best = q.nodes[0];
    for (size_t i = 1; i < q.ntok; i++)
        if (q.nodes[i]->post.count < best->post.count) best = q.nodes[i];
    size_t n = 0;
    const Posting *pb = &best->post;
    for (size_t i = 0; i < pb->count && n < cap; i++) {
        uint64_t doc = pb->docs[i];
        int all = 1;
        for (size_t j = 0; j < q.ntok; j++) {
            if (q.nodes[j] == best) continue;
            if (!posting_contains(&q.nodes[j]->post, doc)) { all = 0; break; }
        }
        if (all) out[n++] = doc;
    }
    if (n > 1) qsort(out, n, sizeof(uint64_t), cmp_u64);
    return n;
}

void rk_index_remove_doc(RkIndex *ix, uint64_t doc) {
    for (size_t bi = 0; bi < ix->nbuckets; bi++) {
        TokNode **pp = &ix->buckets[bi];
        while (*pp) {
            TokNode *n = *pp;
            Posting *p = &n->post;
            size_t w = 0;
            for (size_t i = 0; i < p->count; i++)
                if (p->docs[i] != doc) p->docs[w++] = p->docs[i];
            p->count = w;
            if (p->count == 0) {
                *pp = n->hnext;
                free(n->tok);
                free(n->post.docs);
                free(n);
                ix->tok_count--;
            } else {
                pp = &n->hnext;
            }
        }
    }
    if (ix->doc_count > 0) ix->doc_count--;
}

size_t rk_index_token_count(const RkIndex *ix) { return ix->tok_count; }
size_t rk_index_doc_count(const RkIndex *ix) { return ix->doc_count; }
