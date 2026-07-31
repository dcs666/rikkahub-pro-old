#ifndef RIKKA_DATA_RBIN_H
#define RIKKA_DATA_RBIN_H

#include <stddef.h>
#include <stdint.h>
#include "rikka/core/message.h"
#include "rikka/util/arena.h"

/*
 * 二进制会话存储（S4 数据层核心）。
 * 对标 JVM 版 Room + kotlinx.serialization JSON 全量反序列化（打开长对话几百 ms）；
 * 本实现：紧凑二进制 + mmap 零拷贝加载（parts 数据直接指向 mmap 区），
 * 打开 1 万消息会话目标 <5ms。
 *
 * 格式（小端）：
 *   magic[8]="RIKKABIN" u32 version u32 msg_count
 *   每消息: u8 role u32 part_count [part...] u8 has_usage u64*3 u32 flags
 *   part: u8 type u32 len bytes u8 has_name u16 name_len bytes u8 has_id u16 id_len bytes u8 is_error
 * 节点顺序 = 激活链顺序（时间序）；树结构由调用方重建。
 */

/* 序列化激活链消息到 out（二进制）。返回 0 成功 */
int rbin_save(const RConversation *c, Buf *out);

/* 从二进制加载到 arena；parts 的 data 指向输入缓冲（零拷贝）。返回 0 成功 */
int rbin_load(const uint8_t *data, size_t len, Arena *arena,
              RikkaMessage ***msgs_out, size_t *count_out);

/* 文件快照（写临时文件 + rename 原子替换）。返回 0 成功 */
int rbin_save_file(const RConversation *c, const char *path);

/* mmap 加载：返回 mmap 区（需 rbin_munmap 释放）；*msgs 指向区内数据（零拷贝） */
int rbin_mmap_file(const char *path, const uint8_t **data_out, size_t *len_out);
void rbin_munmap(const uint8_t *data, size_t len);
int rbin_parse(const uint8_t *data, size_t len, Arena *arena,
               RikkaMessage ***msgs_out, size_t *count_out);

#endif /* RIKKA_DATA_RBIN_H */
