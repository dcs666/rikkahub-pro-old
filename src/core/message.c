#include "rikka/core/message.h"
#include <stdlib.h>
#include <string.h>

RikkaMessage *rmsg_new(Arena *arena, RikkaRole role) {
    RikkaMessage *m = (RikkaMessage *)arena_alloc0(arena, 8, sizeof(RikkaMessage));
    if (!m) return NULL;
    m->role = role;
    return m;
}

RikkaPart *rmsg_add_part(Arena *arena, RikkaMessage *m, RikkaPartType type) {
    if (!m || !arena) return NULL;
    if (m->part_count == m->part_cap) {
        size_t nc = m->part_cap ? m->part_cap * 2 : 4;
        RikkaPart *np = (RikkaPart *)arena_alloc(arena, 8, nc * sizeof(RikkaPart));
        if (!np) return NULL;
        if (m->parts) memcpy(np, m->parts, m->part_count * sizeof(RikkaPart));
        m->parts = np;
        m->part_cap = nc;
    }
    RikkaPart *p = &m->parts[m->part_count++];
    memset(p, 0, sizeof(RikkaPart));
    p->type = type;
    return p;
}

/* ---------- 流式累积 ---------- */

void rstream_init(RikkaStream *s, Arena *arena, RikkaRole role) {
    memset(s, 0, sizeof(RikkaStream));
    s->arena = arena;
    s->msg = rmsg_new(arena, role);
    buf_init(&s->text_buf);
    buf_init(&s->reasoning_buf);
    s->active = 1;
}

static void ensure_text_part(RikkaStream *s) {
    if (s->msg->part_count == 0 || s->msg->parts[s->msg->part_count - 1].type != RIKKA_PART_TEXT) {
        RikkaPart *p = rmsg_add_part(s->arena, s->msg, RIKKA_PART_TEXT);
        p->data = (const char *)s->text_buf.data;
        p->len = s->text_buf.len;
        s->text_part_idx = s->msg->part_count - 1;
    } else {
        s->text_part_idx = s->msg->part_count - 1;
    }
}

void rstream_append_text(RikkaStream *s, const char *data, size_t len) {
    if (!s->active || len == 0) return;
    ensure_text_part(s);
    /* O(1) 摊销 append；part 指针同步更新（零拷贝：不重建字符串） */
    buf_append(&s->text_buf, data, len);
    RikkaPart *p = &s->msg->parts[s->text_part_idx];
    p->data = (const char *)s->text_buf.data;
    p->len = s->text_buf.len;
}

static void ensure_reasoning_part(RikkaStream *s) {
    if (s->msg->part_count == 0 || s->msg->parts[s->msg->part_count - 1].type != RIKKA_PART_REASONING) {
        RikkaPart *p = rmsg_add_part(s->arena, s->msg, RIKKA_PART_REASONING);
        p->data = (const char *)s->reasoning_buf.data;
        p->len = s->reasoning_buf.len;
        s->reasoning_part_idx = s->msg->part_count - 1;
    } else {
        s->reasoning_part_idx = s->msg->part_count - 1;
    }
}

void rstream_append_reasoning(RikkaStream *s, const char *data, size_t len) {
    if (!s->active || len == 0) return;
    ensure_reasoning_part(s);
    buf_append(&s->reasoning_buf, data, len);
    RikkaPart *p = &s->msg->parts[s->reasoning_part_idx];
    p->data = (const char *)s->reasoning_buf.data;
    p->len = s->reasoning_buf.len;
}

void rstream_freeze(RikkaStream *s) {
    if (!s->active) return;
    /* 空文本/reasoning 不需要 part：若最后 part 是空的 text，移除 */
    if (s->msg->part_count > 0) {
        RikkaPart *last = &s->msg->parts[s->msg->part_count - 1];
        if ((last->type == RIKKA_PART_TEXT || last->type == RIKKA_PART_REASONING) && last->len == 0) {
            s->msg->part_count--;
        }
    }
    /* 转移缓冲所有权给消息（冻结后消息持有 buf，数据指针保持有效） */
    if (s->text_buf.cap > 0 && s->msg->part_count > 0) {
        s->msg->owned_buf = &s->text_buf;
    }
    s->msg->frozen = 1;
    s->active = 0;
}

void rstream_destroy(RikkaStream *s) {
    if (!s) return;
    if (s->active) {
        /* 未 freeze：释放未转移的缓冲 */
        if (s->msg->owned_buf != &s->text_buf) buf_free(&s->text_buf);
        buf_free(&s->reasoning_buf);
    } else {
        /* 已 freeze：text_buf 已转移给消息（msg->owned_buf），只释放 reasoning */
        buf_free(&s->reasoning_buf);
    }
}

/* ---------- 会话节点树 + COW ---------- */

static void node_add_child(RNode *parent, RNode *child) {
    if (parent->child_count == parent->child_cap) {
        size_t nc = parent->child_cap ? parent->child_cap * 2 : 2;
        RNode **nc2 = (RNode **)realloc(parent->children, nc * sizeof(RNode *));
        if (!nc2) return;
        parent->children = nc2;
        parent->child_cap = nc;
    }
    parent->children[parent->child_count++] = child;
}

RNode *rnode_new(RNode *parent) {
    RNode *n = (RNode *)calloc(1, sizeof(RNode));
    if (!n) return NULL;
    n->parent = parent;
    if (parent) node_add_child(parent, n);
    return n;
}

RNode *rnode_fork(RNode *at) {
    /* COW fork：新节点共享 at 的祖先链，零复制 */
    return rnode_new(at);
}

void rnode_attach(RNode *n, RikkaMessage *frozen) {
    if (!n || !frozen) return;
    n->msg = frozen;
    if (n->stream) {
        rstream_destroy(n->stream);
        free(n->stream);
        n->stream = NULL;
    }
}

const RikkaMessage *rnode_message(const RNode *n) {
    return n ? n->msg : NULL;
}

/* ---------- 会话 ---------- */

void rconv_init(RConversation *c, Arena *arena) {
    memset(c, 0, sizeof(RConversation));
    c->arena = arena;
    c->root = rnode_new(NULL);
    c->active = c->root;
    c->node_count = 1;
}

RNode *rconv_append(RConversation *c, RikkaMessage *frozen) {
    RNode *n = rnode_new(c->active);
    if (!n) return NULL;
    rnode_attach(n, frozen);
    /* 停用旧激活标记，激活新节点 */
    c->active->active = 0;
    n->active = 1;
    c->active = n;
    c->node_count++;
    return n;
}

RNode *rconv_regenerate(RConversation *c, RNode *at) {
    RNode *n = rnode_fork(at);
    if (!n) return NULL;
    c->active->active = 0;
    n->active = 1;
    c->active = n;
    c->node_count++;
    return n;
}

void rconv_set_active(RConversation *c, RNode *n) {
    if (!n) return;
    c->active->active = 0;
    n->active = 1;
    c->active = n;
}

size_t rconv_active_messages(const RConversation *c, const RikkaMessage **out, size_t cap) {
    /* 收集从根到 active 的冻结消息（逆序收集再反转） */
    RNode *path[4096];
    size_t depth = 0;
    RNode *cur = c->active;
    while (cur && cur->parent && depth < 4096) {
        path[depth++] = cur;
        cur = cur->parent;
    }
    size_t n = 0;
    for (size_t i = depth; i > 0 && n < cap; i--) {
        RNode *node = path[i - 1];
        if (node->msg) out[n++] = node->msg;
    }
    return n;
}
