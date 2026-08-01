#ifndef RIKKA_DATA_STORE_H
#define RIKKA_DATA_STORE_H

#include <stddef.h>
#include <stdint.h>

/*
 * 轻量记录存储（对标 JVM 版 Room 的 Favorite/Folder/GenMedia/ManagedFile 实体）。
 *
 * 与 rbin（会话消息流式存储）分工：本模块管**元数据实体**——收藏、会话文件夹、
 * 生成媒体、托管文件。紧凑二进制快照 + 内存数组，规模假设 <1 万条/类
 * （线性扫描 μs 级）；如需更大规模再升级哈希索引。
 *
 * 字段槽位约定（字符串槽位 s[8] / 整数槽位 i[6]）：
 *   RK_ENT_FAVORITE:     s0=id(字符串主键) s1=type s2=ref_key(唯一) s3=ref_json
 *                        s4=snapshot_json s5=meta_json  i0=created_at i1=updated_at
 *   RK_ENT_FOLDER:       s0=id(字符串主键) s1=assistant_id s2=name
 *                        i0=sort_index i1=create_at
 *   RK_ENT_GEN_MEDIA:    s0=path s1=model_id s2=prompt s3=type s4=source_paths
 *                        i0=create_at   （自增 id）
 *   RK_ENT_MANAGED_FILE: s0=folder s1=relative_path(唯一) s2=display_name s3=mime_type
 *                        i0=size_bytes i1=created_at i2=updated_at   （自增 id）
 *
 * 快照格式（小端）：
 *   magic[8]="RKSTORE" u32 version u32 type_mask
 *   每类型段: u8 type u32 count [record...]
 *   record: i64 id u16 slot_mask（低 8 位=s 槽，高 6 位=i 槽）
 *           [u32 len bytes]...（存在且非 NULL 的 s 槽）[i64]...（存在的 i 槽）
 */

typedef enum {
    RK_ENT_FAVORITE = 0,
    RK_ENT_FOLDER,
    RK_ENT_GEN_MEDIA,
    RK_ENT_MANAGED_FILE,
    RK_ENT_COUNT,
} RkEntType;

typedef struct {
    RkEntType type;
    int64_t id;          /* 自增主键（0 = 未分配） */
    const char *s[8];    /* 字符串槽位（可为 NULL） */
    int64_t i[6];        /* 整数槽位（未使用填 0） */
} RkEnt;

/* 槽位常量 */
enum {
    RK_ENT_FAV_ID = 0, RK_ENT_FAV_TYPE, RK_ENT_FAV_REF_KEY, RK_ENT_FAV_REF_JSON,
    RK_ENT_FAV_SNAPSHOT_JSON, RK_ENT_FAV_META_JSON,
    RK_ENT_FAV_CREATED_AT = 0, RK_ENT_FAV_UPDATED_AT,
};
enum {
    RK_ENT_FOLDER_ID = 0, RK_ENT_FOLDER_ASSISTANT_ID, RK_ENT_FOLDER_NAME,
    RK_ENT_FOLDER_SORT_INDEX = 0, RK_ENT_FOLDER_CREATE_AT,
};
enum {
    RK_ENT_MEDIA_PATH = 0, RK_ENT_MEDIA_MODEL_ID, RK_ENT_MEDIA_PROMPT,
    RK_ENT_MEDIA_TYPE, RK_ENT_MEDIA_SOURCE_PATHS,
    RK_ENT_MEDIA_CREATE_AT = 0,
};
enum {
    RK_ENT_FILE_FOLDER = 0, RK_ENT_FILE_REL_PATH, RK_ENT_FILE_DISPLAY_NAME,
    RK_ENT_FILE_MIME,
    RK_ENT_FILE_SIZE = 0, RK_ENT_FILE_CREATED_AT, RK_ENT_FILE_UPDATED_AT,
};

typedef struct RkStore RkStore;

RkStore *rk_store_create(void);
void rk_store_destroy(RkStore *s);

/* 快照（临时文件 + rename 原子替换）；返回 0 成功 */
int rk_store_save_file(const RkStore *s, const char *path);
int rk_store_load_file(RkStore *s, const char *path); /* 追加合并（可先 create 空） */

/* CRUD：insert 返回分配的 id（-1 失败：唯一约束冲突等） */
int64_t rk_store_insert(RkStore *s, const RkEnt *e);
int rk_store_update(RkStore *s, const RkEnt *e);     /* 按 id 更新；0 成功，-1 不存在/冲突 */
int rk_store_delete(RkStore *s, RkEntType t, int64_t id);
const RkEnt *rk_store_get(RkStore *s, RkEntType t, int64_t id); /* 自增 id；NULL 不存在 */
/* 读取深拷贝（返回的实体归调用方：s 槽位需 free；update 写回的安全模式） */
int rk_store_get_copy(RkStore *s, RkEntType t, int64_t id, RkEnt *out);
/* 释放 rk_store_get_copy 产出的实体字符串槽位 */
void rk_store_ent_free(RkEnt *e);
size_t rk_store_count(RkStore *s, RkEntType t);
const RkEnt *rk_store_at(RkStore *s, RkEntType t, size_t idx);  /* 插入序 */

/* 唯一字符串索引查询（favorite.ref_key、managed_file.relative_path 等）；
 * slot 为 s 槽位下标；NULL 表示不存在 */
const RkEnt *rk_store_find_str(RkStore *s, RkEntType t, int slot, const char *val);

/* 按整数槽位区间查询：返回匹配数（out 容量须 >= 返回数，可为 NULL 只计数） */
size_t rk_store_query_i64(RkStore *s, RkEntType t, int slot,
                          int64_t lo, int64_t hi, const RkEnt **out, size_t cap);

#endif /* RIKKA_DATA_STORE_H */
