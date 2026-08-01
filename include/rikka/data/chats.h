#ifndef RIKKA_DATA_CHATS_H
#define RIKKA_DATA_CHATS_H

#include <stddef.h>
#include <stdint.h>

/*
 * 会话元数据索引（对标 JVM ConversationRepository 的 recent/search 能力）。
 *
 * 实现：rk_store（RK_ENT_CHAT 记录）+ rk_index（全文倒排）。
 * 结果 JSON 与 JVM ConversationTools 对齐：
 *   recent: [{"id","title","last_chat":"YYYY-MM-DD"},...]（pinned 优先 + 时间降序）
 *   search: [{"id","title","snippet"(含 [kw] 高亮),"date"},...]
 */

typedef struct RkChatIndex RkChatIndex;

RkChatIndex *rk_chats_create(void);
void rk_chats_destroy(RkChatIndex *ci);

/* upsert：id 存在则更新（含全文索引重建）；content 为会话全文（可 NULL）。 */
int rk_chats_upsert(RkChatIndex *ci, const char *id, const char *title,
                    const char *content, int64_t updated_at, int pinned);
int rk_chats_remove(RkChatIndex *ci, const char *id);

/* recent：pinned 优先 + updated_at 降序。返回 JSON 数组字符串（malloc）。 */
char *rk_chats_recent(RkChatIndex *ci, int limit);

/* search：关键词 AND 全文搜索。返回 JSON 数组字符串（malloc）；无命中返回 "[]"。 */
char *rk_chats_search(RkChatIndex *ci, const char *query);

/* 快照（复用 rk_store 格式，原子写） */
int rk_chats_save_file(RkChatIndex *ci, const char *path);
int rk_chats_load_file(RkChatIndex *ci, const char *path);

#endif /* RIKKA_DATA_CHATS_H */
