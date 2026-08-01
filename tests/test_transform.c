#define _DEFAULT_SOURCE
#include "test.h"
#include "rikka/ai/transform.h"
#include "rikka/core/buffer.h"
#include <stdio.h>
#include <string.h>

/* ---------- 辅助 ---------- */

static RikkaMessage *t_msg(Arena *a, RikkaRole role, const char *text) {
    RikkaMessage *m = rmsg_new(a, role);
    RikkaPart *p = rmsg_add_part(a, m, RIKKA_PART_TEXT);
    p->data = text;
    p->len = strlen(text);
    return m;
}

static const char *first_text(const RkMsgList *l, size_t idx) {
    if (idx >= l->count) return NULL;
    for (size_t i = 0; i < l->items[idx]->part_count; i++) {
        if (l->items[idx]->parts[i].type == RIKKA_PART_TEXT) {
            return l->items[idx]->parts[i].data;
        }
    }
    return NULL;
}

/* ---------- 消息列表 ---------- */

TEST(transform_msgl_basic) {
    Arena *a = arena_create(0);
    const RikkaMessage *src[3];
    src[0] = t_msg(a, RIKKA_ROLE_USER, "u1");
    src[1] = t_msg(a, RIKKA_ROLE_ASSISTANT, "a1");
    src[2] = t_msg(a, RIKKA_ROLE_USER, "u2");
    RkMsgList l;
    rk_msgl_from(&l, a, src, 3);
    ASSERT_EQ_INT(3, (int)l.count);
    /* 追加文本（合并到最后 TEXT） */
    RikkaMessage *m = l.items[2];
    rk_msgl_append_text(&l, m, "X");
    ASSERT(strcmp(first_text(&l, 2), "u2X") == 0);
    /* 插入 */
    RikkaMessage *ins = t_msg(a, RIKKA_ROLE_SYSTEM, "sys");
    rk_msgl_insert(&l, 0, ins);
    ASSERT_EQ_INT(4, (int)l.count);
    ASSERT(strcmp(first_text(&l, 0), "sys") == 0);
    /* 追加消息 */
    RikkaMessage *tail = rk_msgl_add(&l, RIKKA_ROLE_USER);
    RikkaPart *p = rk_msgl_add_part(&l, tail, RIKKA_PART_TEXT);
    p->data = "tail";
    p->len = 4;
    ASSERT_EQ_INT(5, (int)l.count);
    ASSERT(strcmp(first_text(&l, 4), "tail") == 0);
    arena_destroy(a);
}

/* ---------- 1. 时间提醒 ---------- */

TEST(transform_time_reminder) {
    Arena *a = arena_create(0);
    const RikkaMessage *src[3];
    src[0] = t_msg(a, RIKKA_ROLE_USER, "u1");
    ((RikkaMessage *)src[0])->created_at = 1000;
    src[1] = t_msg(a, RIKKA_ROLE_USER, "u2");
    ((RikkaMessage *)src[1])->created_at = 2000;
    src[2] = t_msg(a, RIKKA_ROLE_USER, "u3");
    ((RikkaMessage *)src[2])->created_at = 100000;
    RkMsgList l;
    rk_msgl_from(&l, a, src, 3);
    /* COW：记录变换前的原文本 */
    ASSERT(strcmp(first_text(&l, 0), "u1") == 0);
    rk_transform_time_reminder(&l, 0);
    ASSERT_EQ_INT(5, (int)l.count);
    /* 首条提醒（无 gap） */
    ASSERT_EQ_INT(RIKKA_ROLE_USER, l.items[0]->role);
    const char *r1 = first_text(&l, 0);
    ASSERT(strstr(r1, "<time_reminder>Current time:") != NULL);
    ASSERT(strstr(r1, "since last message") == NULL);
    /* u1、u2 原样 */
    ASSERT(strcmp(first_text(&l, 1), "u1") == 0);
    ASSERT(strcmp(first_text(&l, 2), "u2") == 0);
    /* 第三条前注入带 gap（100000-2000=98000s → 1 d，JVM formatGap 同语义） */
    const char *r3 = first_text(&l, 3);
    ASSERT(strstr(r3, "(1 d since last message)") != NULL);
    ASSERT(strcmp(first_text(&l, 4), "u3") == 0);
    /* COW：原冻结消息未被修改 */
    ASSERT(strcmp(src[0]->parts[0].data, "u1") == 0);
    arena_destroy(a);
}

TEST(transform_time_reminder_small_gap) {
    Arena *a = arena_create(0);
    const RikkaMessage *src[2];
    src[0] = t_msg(a, RIKKA_ROLE_USER, "u1");
    ((RikkaMessage *)src[0])->created_at = 1000;
    src[1] = t_msg(a, RIKKA_ROLE_USER, "u2");
    ((RikkaMessage *)src[1])->created_at = 2000; /* 间隔 1000s < 1h */
    RkMsgList l;
    rk_msgl_from(&l, a, src, 2);
    rk_transform_time_reminder(&l, 0);
    ASSERT_EQ_INT(3, (int)l.count); /* 只有首条提醒 */
    ASSERT(strstr(first_text(&l, 1), "u1") != NULL);
    arena_destroy(a);
}

TEST(transform_time_reminder_gap_chain) {
    /* 时间链：a(1000) → b(8200) 间隔 7200s(2h) 注入；b → c(11800) 间隔 3600s 不注入 */
    Arena *a = arena_create(0);
    const RikkaMessage *src[3];
    src[0] = t_msg(a, RIKKA_ROLE_USER, "a");
    ((RikkaMessage *)src[0])->created_at = 1000;
    src[1] = t_msg(a, RIKKA_ROLE_USER, "b");
    ((RikkaMessage *)src[1])->created_at = 8200;
    src[2] = t_msg(a, RIKKA_ROLE_USER, "c");
    ((RikkaMessage *)src[2])->created_at = 11800;
    RkMsgList l;
    rk_msgl_from(&l, a, src, 3);
    rk_transform_time_reminder(&l, 0);
    ASSERT_EQ_INT(5, (int)l.count); /* 首条 + b 前各一 */
    ASSERT(strstr(first_text(&l, 2), "(2 h since last message)") != NULL);
    /* c 前无注入（gap 恰好 3600 不触发） */
    ASSERT(strcmp(first_text(&l, 4), "c") == 0);
    arena_destroy(a);
}

TEST(transform_time_reminder_unknown_time) {
    /* 首条消息时间未知（0）：仍注入，用 now_epoch */
    Arena *a = arena_create(0);
    const RikkaMessage *src[1];
    src[0] = t_msg(a, RIKKA_ROLE_USER, "u1");
    RkMsgList l;
    rk_msgl_from(&l, a, src, 1);
    rk_transform_time_reminder(&l, 1000000000);
    ASSERT_EQ_INT(2, (int)l.count);
    ASSERT(strstr(first_text(&l, 0), "<time_reminder>") != NULL);
    arena_destroy(a);
}

/* ---------- 2. 提示词注入 ---------- */

static const RkInjection *mk_inj(Arena *a, const char *id, int prio, RkInjPosition pos,
                                 const char *content, int depth, RikkaRole role) {
    RkInjection *e = (RkInjection *)arena_alloc0(a, sizeof(void *), sizeof(RkInjection));
    e->id = id;
    e->name = id;
    e->enabled = 1;
    e->priority = prio;
    e->position = pos;
    e->content = content;
    e->inject_depth = depth;
    e->role = role;
    return e;
}

TEST(transform_injection_after_system) {
    Arena *a = arena_create(0);
    const RikkaMessage *src[3];
    src[0] = t_msg(a, RIKKA_ROLE_SYSTEM, "sys");
    src[1] = t_msg(a, RIKKA_ROLE_USER, "u1");
    src[2] = t_msg(a, RIKKA_ROLE_USER, "u2");
    RkMsgList l;
    rk_msgl_from(&l, a, src, 3);
    const RkInjection *modes[2];
    modes[0] = mk_inj(a, "m1", 10, RK_INJ_AFTER_SYSTEM_PROMPT, "AAA", 4, RIKKA_ROLE_USER);
    modes[1] = mk_inj(a, "m2", 20, RK_INJ_AFTER_SYSTEM_PROMPT, "BBB", 4, RIKKA_ROLE_USER);
    const RkTransformConfig cfg = {0};
    rk_transform_prompt_injection(&l, &cfg, modes, 2, NULL, 0);
    ASSERT_EQ_INT(3, (int)l.count); /* 消息数不变（并入 system） */
    const char *sys = first_text(&l, 0);
    ASSERT(strcmp(sys, "sys\nBBB\nAAA") == 0); /* after_system = 原文本后，优先级降序 */
    arena_destroy(a);
}

TEST(transform_injection_before_after) {
    Arena *a = arena_create(0);
    const RikkaMessage *src[1];
    src[0] = t_msg(a, RIKKA_ROLE_SYSTEM, "sys");
    RkMsgList l;
    rk_msgl_from(&l, a, src, 1);
    const RkInjection *modes[2];
    modes[0] = mk_inj(a, "m1", 5, RK_INJ_BEFORE_SYSTEM_PROMPT, "PRE", 4, RIKKA_ROLE_USER);
    modes[1] = mk_inj(a, "m2", 5, RK_INJ_AFTER_SYSTEM_PROMPT, "POST", 4, RIKKA_ROLE_USER);
    const RkTransformConfig cfg = {0};
    rk_transform_prompt_injection(&l, &cfg, modes, 2, NULL, 0);
    ASSERT(strcmp(first_text(&l, 0), "PRE\nsys\nPOST") == 0);
    arena_destroy(a);
}

TEST(transform_injection_no_system) {
    Arena *a = arena_create(0);
    const RikkaMessage *src[2];
    src[0] = t_msg(a, RIKKA_ROLE_USER, "u1");
    src[1] = t_msg(a, RIKKA_ROLE_USER, "u2");
    RkMsgList l;
    rk_msgl_from(&l, a, src, 2);
    const RkInjection *modes[1];
    modes[0] = mk_inj(a, "m1", 5, RK_INJ_AFTER_SYSTEM_PROMPT, "POST", 4, RIKKA_ROLE_USER);
    const RkTransformConfig cfg = {0};
    rk_transform_prompt_injection(&l, &cfg, modes, 1, NULL, 0);
    ASSERT_EQ_INT(3, (int)l.count);
    ASSERT_EQ_INT(RIKKA_ROLE_SYSTEM, l.items[0]->role);
    ASSERT(strcmp(first_text(&l, 0), "POST") == 0);
    arena_destroy(a);
}

TEST(transform_injection_top_bottom) {
    Arena *a = arena_create(0);
    const RikkaMessage *src[3];
    src[0] = t_msg(a, RIKKA_ROLE_SYSTEM, "sys");
    src[1] = t_msg(a, RIKKA_ROLE_USER, "u1");
    src[2] = t_msg(a, RIKKA_ROLE_USER, "u2");
    RkMsgList l;
    rk_msgl_from(&l, a, src, 3);
    const RkInjection *modes[2];
    modes[0] = mk_inj(a, "m1", 5, RK_INJ_TOP_OF_CHAT, "TOP", 4, RIKKA_ROLE_USER);
    modes[1] = mk_inj(a, "m2", 5, RK_INJ_BOTTOM_OF_CHAT, "BOT", 4, RIKKA_ROLE_USER);
    const RkTransformConfig cfg = {0};
    rk_transform_prompt_injection(&l, &cfg, modes, 2, NULL, 0);
    ASSERT_EQ_INT(5, (int)l.count);
    /* TOP 在第一条 user 前（system 后） */
    ASSERT_EQ_INT(RIKKA_ROLE_USER, l.items[1]->role);
    ASSERT(strcmp(first_text(&l, 1), "TOP") == 0);
    /* BOT 在最后一条消息前 */
    ASSERT(strcmp(first_text(&l, 3), "BOT") == 0);
    ASSERT(strcmp(first_text(&l, 4), "u2") == 0);
    arena_destroy(a);
}

TEST(transform_injection_at_depth) {
    Arena *a = arena_create(0);
    const RikkaMessage *src[4];
    src[0] = t_msg(a, RIKKA_ROLE_SYSTEM, "sys");
    src[1] = t_msg(a, RIKKA_ROLE_USER, "u1");
    src[2] = t_msg(a, RIKKA_ROLE_ASSISTANT, "a1");
    src[3] = t_msg(a, RIKKA_ROLE_USER, "u2");
    RkMsgList l;
    rk_msgl_from(&l, a, src, 4);
    const RkInjection *modes[2];
    modes[0] = mk_inj(a, "d1", 5, RK_INJ_AT_DEPTH, "D1", 1, RIKKA_ROLE_USER);
    modes[1] = mk_inj(a, "d2", 5, RK_INJ_AT_DEPTH, "D2", 2, RIKKA_ROLE_USER);
    const RkTransformConfig cfg = {0};
    rk_transform_prompt_injection(&l, &cfg, modes, 2, NULL, 0);
    ASSERT_EQ_INT(6, (int)l.count);
    /* D2(depth2) 插到倒数第二条前（u1 与 a1 之间） */
    ASSERT(strcmp(first_text(&l, 2), "D2") == 0);
    /* D1(depth1) 插到最后一条前（u2 前） */
    ASSERT(strcmp(first_text(&l, 4), "D1") == 0);
    ASSERT(strcmp(first_text(&l, 5), "u2") == 0);
    arena_destroy(a);
}

TEST(transform_injection_safe_insert) {
    /* 场景 1：目标在 u2 前（prev=a1 非 USER）→ 不移动，直接插入 */
    Arena *a = arena_create(0);
    const RikkaMessage *src[4];
    src[0] = t_msg(a, RIKKA_ROLE_SYSTEM, "sys");
    src[1] = t_msg(a, RIKKA_ROLE_USER, "u1");
    RikkaMessage *ast = t_msg(a, RIKKA_ROLE_ASSISTANT, "a1");
    RikkaPart *tc = rmsg_add_part(a, ast, RIKKA_PART_TOOL_CALL);
    tc->tool_name = "t";
    tc->tool_id = "1";
    src[2] = ast;
    src[3] = t_msg(a, RIKKA_ROLE_USER, "u2");
    RkMsgList l;
    rk_msgl_from(&l, a, src, 4);
    const RkInjection *modes[1];
    modes[0] = mk_inj(a, "m1", 5, RK_INJ_BOTTOM_OF_CHAT, "X", 4, RIKKA_ROLE_USER);
    const RkTransformConfig cfg = {0};
    rk_transform_prompt_injection(&l, &cfg, modes, 1, NULL, 0);
    ASSERT_EQ_INT(5, (int)l.count);
    ASSERT(strcmp(first_text(&l, 3), "X") == 0); /* 插在 u2 前（a1 后），无需移动 */
    ASSERT(strcmp(first_text(&l, 4), "u2") == 0);
    /* 场景 2：USER → ASSISTANT(含 tool) 边界，目标被安全前移 */
    const RikkaMessage *src2[3];
    src2[0] = t_msg(a, RIKKA_ROLE_SYSTEM, "sys");
    src2[1] = t_msg(a, RIKKA_ROLE_USER, "u1");
    RikkaMessage *ast2 = t_msg(a, RIKKA_ROLE_ASSISTANT, "a1");
    RikkaPart *tc2 = rmsg_add_part(a, ast2, RIKKA_PART_TOOL_CALL);
    tc2->tool_name = "t";
    tc2->tool_id = "1";
    src2[2] = ast2;
    RkMsgList l2;
    rk_msgl_from(&l2, a, src2, 3);
    rk_transform_prompt_injection(&l2, &cfg, modes, 1, NULL, 0);
    ASSERT_EQ_INT(4, (int)l2.count);
    ASSERT(strcmp(first_text(&l2, 1), "X") == 0); /* 从 u1/a1 边界前移到 u1 前 */
    ASSERT(strcmp(first_text(&l2, 2), "u1") == 0);
    ASSERT_EQ_INT(RIKKA_ROLE_ASSISTANT, l2.items[3]->role);
    arena_destroy(a);
}

TEST(transform_injection_conv_filter) {
    Arena *a = arena_create(0);
    const RikkaMessage *src[1];
    src[0] = t_msg(a, RIKKA_ROLE_SYSTEM, "sys");
    RkMsgList l;
    rk_msgl_from(&l, a, src, 1);
    const RkInjection *modes[2];
    modes[0] = mk_inj(a, "keep", 5, RK_INJ_AFTER_SYSTEM_PROMPT, "KEEP", 4, RIKKA_ROLE_USER);
    modes[1] = mk_inj(a, "drop", 5, RK_INJ_AFTER_SYSTEM_PROMPT, "DROP", 4, RIKKA_ROLE_USER);
    const char *conv_ids[2] = {"keep", NULL};
    RkTransformConfig cfg = {0};
    cfg.conv_mode_injection_ids = conv_ids;
    rk_transform_prompt_injection(&l, &cfg, modes, 2, NULL, 0);
    ASSERT(strcmp(first_text(&l, 0), "sys\nKEEP") == 0);
    arena_destroy(a);
}

TEST(transform_injection_role_merge) {
    /* 同一位置 USER + ASSISTANT 注入 → 两条消息（ASSISTANT 在前） */
    Arena *a = arena_create(0);
    const RikkaMessage *src[1];
    src[0] = t_msg(a, RIKKA_ROLE_SYSTEM, "sys");
    RkMsgList l;
    rk_msgl_from(&l, a, src, 1);
    const RkInjection *modes[3];
    modes[0] = mk_inj(a, "u1", 5, RK_INJ_TOP_OF_CHAT, "UA", 4, RIKKA_ROLE_USER);
    modes[1] = mk_inj(a, "a1", 5, RK_INJ_TOP_OF_CHAT, "AA", 4, RIKKA_ROLE_ASSISTANT);
    modes[2] = mk_inj(a, "u2", 5, RK_INJ_TOP_OF_CHAT, "UB", 4, RIKKA_ROLE_USER);
    const RkTransformConfig cfg = {0};
    rk_transform_prompt_injection(&l, &cfg, modes, 3, NULL, 0);
    ASSERT_EQ_INT(3, (int)l.count); /* sys + assistant + user 合并各一条 */
    ASSERT_EQ_INT(RIKKA_ROLE_ASSISTANT, l.items[1]->role);
    ASSERT(strcmp(first_text(&l, 1), "AA") == 0);
    ASSERT_EQ_INT(RIKKA_ROLE_USER, l.items[2]->role);
    ASSERT(strcmp(first_text(&l, 2), "UA\nUB") == 0);
    arena_destroy(a);
}

TEST(transform_injection_lorebook) {
    Arena *a = arena_create(0);
    const RikkaMessage *src[3];
    src[0] = t_msg(a, RIKKA_ROLE_SYSTEM, "sys");
    src[1] = t_msg(a, RIKKA_ROLE_USER, "hello world");
    src[2] = t_msg(a, RIKKA_ROLE_USER, "nothing here");
    RkMsgList l;
    rk_msgl_from(&l, a, src, 3);
    /* 条目：关键词 HELLO（icase 触发）、regex（^magic 触发）、constant、未触发 */
    RkInjection *e1 = (RkInjection *)arena_alloc0(a, sizeof(void *), sizeof(RkInjection));
    e1->id = "e1";
    e1->enabled = 1;
    e1->position = RK_INJ_AFTER_SYSTEM_PROMPT;
    e1->content = "L1";
    e1->role = RIKKA_ROLE_USER;
    e1->scan_depth = 4;
    const char *kw1[2] = {"HELLO", NULL};
    e1->keywords = kw1;
    RkInjection *e2 = (RkInjection *)arena_alloc0(a, sizeof(void *), sizeof(RkInjection));
    e2->id = "e2";
    e2->enabled = 1;
    e2->position = RK_INJ_AFTER_SYSTEM_PROMPT;
    e2->content = "L2";
    e2->role = RIKKA_ROLE_USER;
    e2->scan_depth = 4;
    const char *kw2[2] = {"^magic", NULL};
    e2->keywords = kw2;
    e2->use_regex = 1;
    RkInjection *e3 = (RkInjection *)arena_alloc0(a, sizeof(void *), sizeof(RkInjection));
    e3->id = "e3";
    e3->enabled = 1;
    e3->position = RK_INJ_AFTER_SYSTEM_PROMPT;
    e3->content = "L3";
    e3->role = RIKKA_ROLE_USER;
    e3->scan_depth = 4;
    e3->constant_active = 1;
    RkInjection *e4 = (RkInjection *)arena_alloc0(a, sizeof(void *), sizeof(RkInjection));
    e4->id = "e4";
    e4->enabled = 1;
    e4->position = RK_INJ_AFTER_SYSTEM_PROMPT;
    e4->content = "L4";
    e4->role = RIKKA_ROLE_USER;
    e4->scan_depth = 4;
    const char *kw4[2] = {"nomatch", NULL};
    e4->keywords = kw4;
    RkLorebook *lb = (RkLorebook *)arena_alloc0(a, sizeof(void *), sizeof(RkLorebook));
    lb->id = "lb1";
    lb->enabled = 1;
    const RkInjection *entries[4] = {e1, e2, e3, e4};
    lb->entries = entries;
    lb->entry_count = 4;
    const RkLorebook *lbs[1] = {lb};
    const RkTransformConfig cfg = {0};
    rk_transform_prompt_injection(&l, &cfg, NULL, 0, lbs, 1);
    /* e1(HELLO icase) + e3(constant) 触发；e2(^magic 不匹配) e4(关键词未出现) 不触发 */
    ASSERT(strcmp(first_text(&l, 0), "sys\nL1\nL3") == 0);
    arena_destroy(a);
}

TEST(transform_injection_lorebook_scan_depth) {
    /* scan_depth=1：只看最后一条（"nothing"），e1 不触发 */
    Arena *a = arena_create(0);
    const RikkaMessage *src[3];
    src[0] = t_msg(a, RIKKA_ROLE_SYSTEM, "sys");
    src[1] = t_msg(a, RIKKA_ROLE_USER, "hello world");
    src[2] = t_msg(a, RIKKA_ROLE_USER, "nothing here");
    RkMsgList l;
    rk_msgl_from(&l, a, src, 3);
    RkInjection *e1 = (RkInjection *)arena_alloc0(a, sizeof(void *), sizeof(RkInjection));
    e1->id = "e1";
    e1->enabled = 1;
    e1->position = RK_INJ_AFTER_SYSTEM_PROMPT;
    e1->content = "L1";
    e1->role = RIKKA_ROLE_USER;
    e1->scan_depth = 1;
    const char *kw1[2] = {"hello", NULL};
    e1->keywords = kw1;
    RkLorebook *lb = (RkLorebook *)arena_alloc0(a, sizeof(void *), sizeof(RkLorebook));
    lb->id = "lb1";
    lb->enabled = 1;
    const RkInjection *entries[1] = {e1};
    lb->entries = entries;
    lb->entry_count = 1;
    const RkLorebook *lbs[1] = {lb};
    const RkTransformConfig cfg = {0};
    rk_transform_prompt_injection(&l, &cfg, NULL, 0, lbs, 1);
    ASSERT(strcmp(first_text(&l, 0), "sys") == 0); /* 无注入 */
    arena_destroy(a);
}

/* ---------- 3. 占位符 ---------- */

TEST(transform_placeholder) {
    Arena *a = arena_create(0);
    RkTransformConfig cfg = {0};
    cfg.model_id = "gpt-4o";
    cfg.model_name = "GPT-4o";
    cfg.locale_name = "en-US";
    cfg.tz_name = "Asia/Shanghai";
    cfg.os_version = "1.2.3";
    cfg.device_brand = "Acme";
    cfg.device_model = "Phone X";
    cfg.battery_pct = 42;
    cfg.user_nickname = "nick";
    cfg.assistant_name = "charA";
    const RikkaMessage *src[1];
    src[0] = t_msg(a, RIKKA_ROLE_USER,
                   "{{model_id}} {model_name} {{LOCALE}} {TimeZone} "
                   "{{system_version}} {{device_info}} {{battery_level}} "
                   "{{nickname}} {{char}} {{user}} {{cur_date}}");
    RkMsgList l;
    rk_msgl_from(&l, a, src, 1);
    rk_transform_placeholder(&l, &cfg);
    const char *t = first_text(&l, 0);
    ASSERT(strstr(t, "gpt-4o") != NULL);
    ASSERT(strstr(t, "GPT-4o") != NULL);
    ASSERT(strstr(t, "en-US") != NULL);
    ASSERT(strstr(t, "Asia/Shanghai") != NULL);
    ASSERT(strstr(t, "1.2.3") != NULL);
    ASSERT(strstr(t, "Acme Phone X") != NULL);
    ASSERT(strstr(t, "42") != NULL);
    ASSERT(strstr(t, "nick") != NULL);
    ASSERT(strstr(t, "charA") != NULL);
    ASSERT(strstr(t, "{{cur_date}}") == NULL);
    /* 未知键保留 */
    src[0] = t_msg(a, RIKKA_ROLE_USER, "{{unknown_key}}");
    rk_msgl_from(&l, a, src, 1);
    rk_transform_placeholder(&l, &cfg);
    ASSERT(strcmp(first_text(&l, 0), "{{unknown_key}}") == 0);
    arena_destroy(a);
}

/* ---------- 4. 文档转提示 ---------- */

static const char *doc_reader(const char *mime, const char *path, const char *name, void *ud) {
    (void)mime;
    (void)path;
    (void)name;
    (void)ud;
    return "DOC-CONTENT";
}

static const char *doc_resolver(const char *path, void *ud) {
    (void)ud;
    return path;
}

TEST(transform_document_as_prompt) {
    Arena *a = arena_create(0);
    RikkaMessage *m = t_msg(a, RIKKA_ROLE_USER, "body");
    RikkaPart *d1 = rmsg_add_part(a, m, RIKKA_PART_DOCUMENT);
    d1->data = "/upload/a.docx";
    d1->doc_mime = "application/vnd.openxmlformats-officedocument.wordprocessingml.document";
    d1->doc_name = "a.docx";
    RikkaPart *d2 = rmsg_add_part(a, m, RIKKA_PART_DOCUMENT);
    d2->data = "/upload/b.pdf";
    d2->doc_mime = "application/pdf";
    d2->doc_name = "b.pdf";
    const RikkaMessage *src[1] = {m};
    RkMsgList l;
    rk_msgl_from(&l, a, src, 1);
    rk_transform_document_as_prompt(&l, doc_reader, doc_resolver, NULL);
    /* JVM add(0) 语义：后文档在前；原 parts（含 DOCUMENT 本身）保留 */
    ASSERT_EQ_INT(5, (int)l.items[0]->part_count);
    const RikkaPart *p0 = &l.items[0]->parts[0];
    ASSERT_EQ_INT(RIKKA_PART_TEXT, p0->type);
    const char *t0 = p0->data;
    ASSERT(strstr(t0, "<UploadFile name=\"b.pdf\" path=\"/upload/b.pdf\">") != NULL);
    ASSERT(strstr(t0, "DOC-CONTENT") != NULL);
    const RikkaPart *p1 = &l.items[0]->parts[1];
    ASSERT(strstr(p1->data, "<UploadFile name=\"a.docx\"") != NULL);
    ASSERT(strcmp(l.items[0]->parts[2].data, "body") == 0);
    arena_destroy(a);
}

TEST(transform_document_reader_fail) {
    Arena *a = arena_create(0);
    RikkaMessage *m = t_msg(a, RIKKA_ROLE_USER, "body");
    RikkaPart *d = rmsg_add_part(a, m, RIKKA_PART_DOCUMENT);
    d->data = "/upload/x.txt";
    d->doc_mime = "text/plain";
    d->doc_name = "x.txt";
    const RikkaMessage *src[1] = {m};
    RkMsgList l;
    rk_msgl_from(&l, a, src, 1);
    rk_transform_document_as_prompt(&l, NULL, NULL, NULL); /* reader 缺失 */
    ASSERT(strstr(first_text(&l, 0), "[ERROR, failed to read file: x.txt]") != NULL);
    arena_destroy(a);
}

/* ---------- 5. OCR ---------- */

static const char *ocr_cb(const char *path, void *ud) {
    (void)ud;
    return strstr(path, "fail") ? NULL : "OCR-TEXT";
}

TEST(transform_ocr) {
    Arena *a = arena_create(0);
    RkTransformConfig cfg = {0};
    RikkaMessage *m = t_msg(a, RIKKA_ROLE_USER, "body");
    RikkaPart *img = rmsg_add_part(a, m, RIKKA_PART_IMAGE);
    img->data = "file:/sdcard/pic.png";
    img->doc_name = "pic.png";
    RikkaPart *img2 = rmsg_add_part(a, m, RIKKA_PART_IMAGE);
    img2->data = "https://example.com/pic.png"; /* 非本地不动 */
    const RikkaMessage *src[1] = {m};
    RkMsgList l;
    rk_msgl_from(&l, a, src, 1);
    rk_transform_ocr(&l, &cfg, ocr_cb, NULL);
    ASSERT_EQ_INT(RIKKA_PART_TEXT, l.items[0]->parts[1].type);
    ASSERT(strstr(l.items[0]->parts[1].data, "OCR-TEXT") != NULL);
    ASSERT(strstr(l.items[0]->parts[1].data, "<image_file_ocr>") != NULL);
    ASSERT_EQ_INT(RIKKA_PART_IMAGE, l.items[0]->parts[2].type); /* https 不动 */
    /* 模型支持图片 → 跳过 */
    RkMsgList l2;
    rk_msgl_from(&l2, a, src, 1);
    RkTransformConfig cfg2 = {0};
    cfg2.model_supports_images = 1;
    rk_transform_ocr(&l2, &cfg2, ocr_cb, NULL);
    ASSERT_EQ_INT(RIKKA_PART_IMAGE, l2.items[0]->parts[1].type);
    /* 失败 → [Image] */
    RkTransformConfig cfg3 = {0};
    RkMsgList l4;
    RikkaMessage *m4 = t_msg(a, RIKKA_ROLE_USER, "body");
    RikkaPart *img4 = rmsg_add_part(a, m4, RIKKA_PART_IMAGE);
    img4->data = "file:/sdcard/fail.png";
    const RikkaMessage *src4[1] = {m4};
    rk_msgl_from(&l4, a, src4, 1);
    rk_transform_ocr(&l4, &cfg3, ocr_cb, NULL);
    ASSERT(strstr(l4.items[0]->parts[1].data, "[Image]") != NULL);
    arena_destroy(a);
}

/* ---------- 6. 模板 ---------- */

TEST(transform_template) {
    Arena *a = arena_create(0);
    RikkaMessage *m = t_msg(a, RIKKA_ROLE_USER, "hi");
    m->created_at = 86400; /* 1970-01-02 00:00:00 UTC */
    const RikkaMessage *src[1] = {m};
    RkMsgList l;
    rk_msgl_from(&l, a, src, 1);
    rk_transform_template(&l, "[{{role}}|{{time}}|{{date}}] {{message}} {{missing}}", 0);
    ASSERT(strcmp(first_text(&l, 0), "[user|00:00:00|1970-01-02] hi ") == 0);
    arena_destroy(a);
}

/* ---------- 7. workspace 提醒 ---------- */

TEST(transform_workspace_reminder) {
    Arena *a = arena_create(0);
    const RikkaMessage *src[2];
    src[0] = t_msg(a, RIKKA_ROLE_SYSTEM, "sys");
    src[1] = t_msg(a, RIKKA_ROLE_USER, "u1");
    RkMsgList l;
    rk_msgl_from(&l, a, src, 2);
    RkTransformConfig cfg = {0};
    cfg.workspace_id = "ws1";
    cfg.workspace_cwd = "/workspace/proj";
    rk_transform_workspace_reminder(&l, &cfg);
    ASSERT_EQ_INT(2, (int)l.count);
    const char *sys = first_text(&l, 0);
    ASSERT(strstr(sys, "sys") != NULL);
    ASSERT(strstr(sys, "<workspace>") != NULL);
    ASSERT(strstr(sys, "named \"ws1\"") != NULL);
    ASSERT(strstr(sys, "/workspace/proj") != NULL);
    /* 无 system → 插入 */
    const RikkaMessage *src2[1] = {t_msg(a, RIKKA_ROLE_USER, "u1")};
    RkMsgList l2;
    rk_msgl_from(&l2, a, src2, 1);
    rk_transform_workspace_reminder(&l2, &cfg);
    ASSERT_EQ_INT(2, (int)l2.count);
    ASSERT_EQ_INT(RIKKA_ROLE_SYSTEM, l2.items[0]->role);
    ASSERT(strstr(first_text(&l2, 0), "<workspace>") != NULL);
    /* 无 workspace_id → 不注入 */
    RkMsgList l3;
    rk_msgl_from(&l3, a, src, 2);
    RkTransformConfig cfg3 = {0};
    rk_transform_workspace_reminder(&l3, &cfg3);
    ASSERT_EQ_INT(2, (int)l3.count);
    ASSERT(strcmp(first_text(&l3, 0), "sys") == 0);
    arena_destroy(a);
}

/* ---------- 8. ThinkTag ---------- */

TEST(transform_think_tag) {
    Arena *a = arena_create(0);
    RikkaMessage *m = t_msg(a, RIKKA_ROLE_ASSISTANT, "pre <think> secret plan </think> post");
    const RikkaMessage *src[1] = {m};
    RkMsgList l;
    rk_msgl_from(&l, a, src, 1);
    rk_transform_think_tag(&l);
    ASSERT_EQ_INT(2, (int)l.items[0]->part_count);
    ASSERT_EQ_INT(RIKKA_PART_REASONING, l.items[0]->parts[0].type);
    ASSERT(strcmp(l.items[0]->parts[0].data, "secret plan") == 0);
    ASSERT_EQ_INT(RIKKA_PART_TEXT, l.items[0]->parts[1].type);
    ASSERT(strcmp(l.items[0]->parts[1].data, "pre  post") == 0);
    arena_destroy(a);
}

TEST(transform_think_tag_unclosed) {
    Arena *a = arena_create(0);
    RikkaMessage *m = t_msg(a, RIKKA_ROLE_ASSISTANT, "start <think>still thinking");
    const RikkaMessage *src[1] = {m};
    RkMsgList l;
    rk_msgl_from(&l, a, src, 1);
    rk_transform_think_tag(&l);
    ASSERT_EQ_INT(2, (int)l.items[0]->part_count);
    ASSERT(strcmp(l.items[0]->parts[0].data, "still thinking") == 0);
    ASSERT(strcmp(l.items[0]->parts[1].data, "start ") == 0);
    /* finish 版同样处理 */
    RkMsgList l2;
    rk_msgl_from(&l2, a, src, 1);
    rk_transform_think_tag_finish(&l2, 0);
    ASSERT_EQ_INT(2, (int)l2.items[0]->part_count);
    arena_destroy(a);
}

TEST(transform_think_tag_multi) {
    Arena *a = arena_create(0);
    RikkaMessage *m = t_msg(a, RIKKA_ROLE_ASSISTANT, "a <think>one</think> b <think>two</think> c");
    const RikkaMessage *src[1] = {m};
    RkMsgList l;
    rk_msgl_from(&l, a, src, 1);
    rk_transform_think_tag(&l);
    /* JVM 语义：stripped 移除全部块，reasoning 取第一个 */
    ASSERT_EQ_INT(2, (int)l.items[0]->part_count);
    ASSERT(strcmp(l.items[0]->parts[0].data, "one") == 0);
    ASSERT(strcmp(l.items[0]->parts[1].data, "a  b  c") == 0);
    /* 用户消息不动 */
    RkMsgList l2;
    const RikkaMessage *src2[1] = {t_msg(a, RIKKA_ROLE_USER, "x <think>y</think> z")};
    rk_msgl_from(&l2, a, src2, 1);
    rk_transform_think_tag(&l2);
    ASSERT_EQ_INT(1, (int)l2.items[0]->part_count);
    arena_destroy(a);
}

/* ---------- 9. 正则输出 ---------- */

TEST(transform_regex_output) {
    Arena *a = arena_create(0);
    RkOutputRegex *r1 = (RkOutputRegex *)arena_alloc0(a, sizeof(void *), sizeof(RkOutputRegex));
    r1->enabled = 1;
    r1->find_regex = "foo";
    r1->replace_string = "bar";
    r1->affects_assistant = 1;
    RkOutputRegex *r2 = (RkOutputRegex *)arena_alloc0(a, sizeof(void *), sizeof(RkOutputRegex));
    r2->enabled = 1;
    r2->find_regex = "(a)(b)";
    r2->replace_string = "$2$1";
    r2->affects_assistant = 1;
    RkOutputRegex *r3 = (RkOutputRegex *)arena_alloc0(a, sizeof(void *), sizeof(RkOutputRegex));
    r3->enabled = 1;
    r3->find_regex = "X";
    r3->replace_string = "Y";
    r3->affects_assistant = 1;
    r3->visual_only = 1; /* 非视觉替换跳过 */
    RkOutputRegex *r4 = (RkOutputRegex *)arena_alloc0(a, sizeof(void *), sizeof(RkOutputRegex));
    r4->enabled = 1;
    r4->find_regex = "BAD[";
    r4->replace_string = "Z";
    r4->affects_assistant = 1; /* 编译失败 → 原串 */
    RkOutputRegex *r5 = (RkOutputRegex *)arena_alloc0(a, sizeof(void *), sizeof(RkOutputRegex));
    r5->enabled = 1;
    r5->find_regex = "a*";
    r5->replace_string = "-";
    r5->affects_assistant = 1; /* 零长匹配不死循环（单独测试） */
    const RkOutputRegex *regexes[4] = {r1, r2, r3, r4};
    RikkaMessage *m = t_msg(a, RIKKA_ROLE_ASSISTANT, "foo ab X aaa");
    const RikkaMessage *src[1] = {m};
    RkMsgList l;
    rk_msgl_from(&l, a, src, 1);
    rk_transform_regex_output(&l, regexes, 4);
    const char *t = first_text(&l, 0);
    ASSERT(strstr(t, "bar") != NULL);       /* foo → bar */
    ASSERT(strstr(t, "ba") != NULL);        /* ab → ba */
    ASSERT(strstr(t, "X") != NULL);         /* visual_only 跳过 */
    /* 用户消息不动 */
    const RikkaMessage *src2[1] = {t_msg(a, RIKKA_ROLE_USER, "foo")};
    RkMsgList l2;
    rk_msgl_from(&l2, a, src2, 1);
    rk_transform_regex_output(&l2, regexes, 4);
    ASSERT(strcmp(first_text(&l2, 0), "foo") == 0);
    /* 零长正则：不死循环（输出含 "-" 即可） */
    const RkOutputRegex *only5[1] = {r5};
    const RikkaMessage *src3[1] = {t_msg(a, RIKKA_ROLE_ASSISTANT, "ba")};
    RkMsgList l3;
    rk_msgl_from(&l3, a, src3, 1);
    rk_transform_regex_output(&l3, only5, 1);
    ASSERT(strstr(first_text(&l3, 0), "-") != NULL);
    arena_destroy(a);
}

TEST(transform_regex_replace_fail) {
    /* 替换串引用不存在的组 → 返回原串（JVM 抛异常 → 原串） */
    Arena *a = arena_create(0);
    char *r = rk_regex_replace(a, "abc", "(a)", "$5", 0);
    ASSERT(strcmp(r, "abc") == 0);
    r = rk_regex_replace(a, "abc", "x", "y", 0);
    ASSERT(strcmp(r, "abc") == 0); /* 无匹配 → 原串 */
    r = rk_regex_replace(a, "ab12", "([a-z]+)([0-9]+)", "$2-$1", 0);
    ASSERT(strcmp(r, "12-ab") == 0);
    r = rk_regex_replace(a, "a$b", "(a)", "$$$1", 0);
    ASSERT(strcmp(r, "$a$b") == 0);
    r = rk_regex_replace(a, "aaaa", "a*", "-", 0);
    ASSERT(strcmp(r, "--") == 0); /* 贪婪匹配整串 + 尾部空匹配，不死循环 */
    arena_destroy(a);
}

/* ---------- 10. base64 图片 ---------- */

static const char *img_saver(const char *data_uri, const char *hint, void *ud) {
    (void)ud;
    (void)hint;
    return strstr(data_uri, "skip") ? NULL : "file:/saved.png";
}

TEST(transform_base64_image) {
    Arena *a = arena_create(0);
    RikkaMessage *m = t_msg(a, RIKKA_ROLE_ASSISTANT, "see");
    RikkaPart *img = rmsg_add_part(a, m, RIKKA_PART_IMAGE);
    img->data = "data:image/png;base64,AAAA";
    RikkaPart *img2 = rmsg_add_part(a, m, RIKKA_PART_IMAGE);
    img2->data = "file:/local.png"; /* 非 data URI 不动 */
    const RikkaMessage *src[1] = {m};
    RkMsgList l;
    rk_msgl_from(&l, a, src, 1);
    rk_transform_base64_image(&l, img_saver, NULL);
    ASSERT(strcmp(l.items[0]->parts[1].data, "file:/saved.png") == 0);
    ASSERT(strcmp(l.items[0]->parts[2].data, "file:/local.png") == 0);
    /* saver 返回 NULL → 不动 */
    RkMsgList l2;
    RikkaMessage *m2 = t_msg(a, RIKKA_ROLE_ASSISTANT, "see");
    RikkaPart *i2 = rmsg_add_part(a, m2, RIKKA_PART_IMAGE);
    i2->data = "data:image/png;base64,skip";
    const RikkaMessage *src2[1] = {m2};
    rk_msgl_from(&l2, a, src2, 1);
    rk_transform_base64_image(&l2, img_saver, NULL);
    ASSERT(strcmp(l2.items[0]->parts[1].data, "data:image/png;base64,skip") == 0);
    arena_destroy(a);
}

/* ---------- 11. 工具 ---------- */

TEST(transform_tools) {
    ASSERT(rk_regex_contains("hello world", "WORLD", 1));
    ASSERT(!rk_regex_contains("hello world", "WORLD", 0));
    ASSERT(rk_regex_contains("abc123", "^abc[0-9]+$", 0));
    ASSERT(!rk_regex_contains("abc", "BAD[", 0)); /* 编译失败 → 不匹配 */
    ASSERT(rk_strcasestr("Hello World", "WORLD") != NULL);
    ASSERT(rk_strcasestr("Hello", "xyz") == NULL);
    Arena *a = arena_create(0);
    const char *names[2] = {"a", "b"};
    const char *values[2] = {"1", "2"};
    char *t = rk_template_render(a, "x{{a}}y{{b}}z{{a}}{{c}}", names, values, 2);
    ASSERT(strcmp(t, "x1y2z1") == 0);
    t = rk_template_render(a, "no vars", names, values, 2);
    ASSERT(strcmp(t, "no vars") == 0);
    t = rk_template_render(a, "open {{a", names, values, 2); /* 未闭合原样 */
    ASSERT(strcmp(t, "open {{a") == 0);
    t = rk_template_render(a, "sp {{ a }}", names, values, 2); /* 变量名 trim */
    ASSERT(strcmp(t, "sp 1") == 0);
    arena_destroy(a);
}

/* ---------- 12. 全管线集成（JVM 版顺序） ---------- */

TEST(transform_pipeline_full) {
    Arena *a = arena_create(0);
    /* 输入管线：time_reminder → prompt_injection → placeholder → document
     * → ocr → template → workspace_reminder */
    const RikkaMessage *src[2];
    RikkaMessage *m0 = t_msg(a, RIKKA_ROLE_SYSTEM, "sys");
    m0->created_at = 0;
    RikkaMessage *m1 = t_msg(a, RIKKA_ROLE_USER, "hello {{model_id}}");
    m1->created_at = 100000;
    RikkaPart *doc = rmsg_add_part(a, m1, RIKKA_PART_DOCUMENT);
    doc->data = "/upload/x.txt";
    doc->doc_mime = "text/plain";
    doc->doc_name = "x.txt";
    src[0] = m0;
    src[1] = m1;
    RkMsgList l;
    rk_msgl_from(&l, a, src, 2);
    RkTransformConfig cfg = {0};
    cfg.model_id = "m1";
    cfg.workspace_id = "ws";
    cfg.workspace_cwd = "/workspace";
    const RkInjection *modes[1];
    modes[0] = mk_inj(a, "i1", 5, RK_INJ_AFTER_SYSTEM_PROMPT, "INJ", 4, RIKKA_ROLE_USER);
    /* 1. 时间提醒 */
    rk_transform_time_reminder(&l, 0);
    /* 2. 注入 */
    rk_transform_prompt_injection(&l, &cfg, modes, 1, NULL, 0);
    /* 3. 占位符 */
    rk_transform_placeholder(&l, &cfg);
    /* 4. 文档转提示 */
    rk_transform_document_as_prompt(&l, doc_reader, NULL, NULL);
    /* 5. OCR（无图片，跳过） */
    rk_transform_ocr(&l, &cfg, NULL, NULL);
    /* 6. 模板（无 template，跳过） */
    /* 7. workspace 提醒 */
    rk_transform_workspace_reminder(&l, &cfg);
    /* system 消息 = 原文本 + 注入 + workspace 提醒 */
    const char *sys = first_text(&l, 0);
    ASSERT(strstr(sys, "sys") != NULL);
    ASSERT(strstr(sys, "INJ") != NULL);
    ASSERT(strstr(sys, "<workspace>") != NULL);
    /* 用户消息前有时间提醒；占位符被替换；文档块在最前 */
    ASSERT(strstr(first_text(&l, 1), "<time_reminder>") != NULL);
    const RikkaMessage *user_msg = l.items[2];
    ASSERT(strstr(user_msg->parts[0].data, "<UploadFile name=\"x.txt\">") != NULL);
    ASSERT(strstr(user_msg->parts[0].data, "DOC-CONTENT") != NULL);
    ASSERT(strstr(user_msg->parts[1].data, "hello m1") != NULL);
    /* 输出管线：think_tag → base64 → regex */
    RkMsgList out;
    const RikkaMessage *omsgs[1];
    RikkaMessage *om = t_msg(a, RIKKA_ROLE_ASSISTANT, "<think>t</think>done foo");
    RikkaPart *img = rmsg_add_part(a, om, RIKKA_PART_IMAGE);
    img->data = "data:image/png;base64,AAA";
    omsgs[0] = om;
    rk_msgl_from(&out, a, omsgs, 1);
    rk_transform_think_tag(&out);
    rk_transform_base64_image(&out, img_saver, NULL);
    RkOutputRegex *rr = (RkOutputRegex *)arena_alloc0(a, sizeof(void *), sizeof(RkOutputRegex));
    rr->enabled = 1;
    rr->find_regex = "foo";
    rr->replace_string = "bar";
    rr->affects_assistant = 1;
    const RkOutputRegex *rrs[1] = {rr};
    rk_transform_regex_output(&out, rrs, 1);
    RikkaMessage *om_out = out.items[0];
    ASSERT_EQ_INT(3, (int)om_out->part_count); /* reasoning + text + image */
    ASSERT_EQ_INT(RIKKA_PART_REASONING, om_out->parts[0].type);
    ASSERT(strcmp(om_out->parts[0].data, "t") == 0);
    ASSERT(strcmp(om_out->parts[1].data, "done bar") == 0); /* think 剥离 + 正则替换 */
    ASSERT(strcmp(om_out->parts[2].data, "file:/saved.png") == 0);
    arena_destroy(a);
}

int run_transform_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(transform, transform_msgl_basic),
        RIKKA_TEST_REGISTER(transform, transform_time_reminder),
        RIKKA_TEST_REGISTER(transform, transform_time_reminder_small_gap),
        RIKKA_TEST_REGISTER(transform, transform_time_reminder_gap_chain),
        RIKKA_TEST_REGISTER(transform, transform_time_reminder_unknown_time),
        RIKKA_TEST_REGISTER(transform, transform_injection_after_system),
        RIKKA_TEST_REGISTER(transform, transform_injection_before_after),
        RIKKA_TEST_REGISTER(transform, transform_injection_no_system),
        RIKKA_TEST_REGISTER(transform, transform_injection_top_bottom),
        RIKKA_TEST_REGISTER(transform, transform_injection_at_depth),
        RIKKA_TEST_REGISTER(transform, transform_injection_safe_insert),
        RIKKA_TEST_REGISTER(transform, transform_injection_conv_filter),
        RIKKA_TEST_REGISTER(transform, transform_injection_role_merge),
        RIKKA_TEST_REGISTER(transform, transform_injection_lorebook),
        RIKKA_TEST_REGISTER(transform, transform_injection_lorebook_scan_depth),
        RIKKA_TEST_REGISTER(transform, transform_placeholder),
        RIKKA_TEST_REGISTER(transform, transform_document_as_prompt),
        RIKKA_TEST_REGISTER(transform, transform_document_reader_fail),
        RIKKA_TEST_REGISTER(transform, transform_ocr),
        RIKKA_TEST_REGISTER(transform, transform_template),
        RIKKA_TEST_REGISTER(transform, transform_workspace_reminder),
        RIKKA_TEST_REGISTER(transform, transform_think_tag),
        RIKKA_TEST_REGISTER(transform, transform_think_tag_unclosed),
        RIKKA_TEST_REGISTER(transform, transform_think_tag_multi),
        RIKKA_TEST_REGISTER(transform, transform_regex_output),
        RIKKA_TEST_REGISTER(transform, transform_regex_replace_fail),
        RIKKA_TEST_REGISTER(transform, transform_base64_image),
        RIKKA_TEST_REGISTER(transform, transform_tools),
        RIKKA_TEST_REGISTER(transform, transform_pipeline_full),
    };
    return run_suite("transform", tests, sizeof(tests) / sizeof(tests[0]));
}
