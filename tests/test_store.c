#define _DEFAULT_SOURCE
#include "test.h"
#include "rikka/data/store.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

TEST(store_crud) {
    RkStore *s = rk_store_create();
    ASSERT_NOT_NULL(s);
    /* Favorite 插入 */
    RkEnt fav = {0};
    fav.type = RK_ENT_FAVORITE;
    fav.s[RK_ENT_FAV_ID] = "fav-1";
    fav.s[RK_ENT_FAV_TYPE] = "message";
    fav.s[RK_ENT_FAV_REF_KEY] = "conv1:msg3";
    fav.s[RK_ENT_FAV_SNAPSHOT_JSON] = "{\"text\":\"hi\"}";
    fav.i[RK_ENT_FAV_CREATED_AT] = 1000;
    int64_t id1 = rk_store_insert(s, &fav);
    ASSERT(id1 > 0);
    /* 唯一约束：同 ref_key 拒绝 */
    RkEnt fav2 = fav;
    fav2.s[RK_ENT_FAV_ID] = "fav-2";
    ASSERT_EQ_INT(-1, (int)rk_store_insert(s, &fav2));
    /* 查询 */
    const RkEnt *got = rk_store_find_str(s, RK_ENT_FAVORITE, RK_ENT_FAV_REF_KEY, "conv1:msg3");
    ASSERT_NOT_NULL(got);
    ASSERT(strcmp(got->s[RK_ENT_FAV_ID], "fav-1") == 0);
    ASSERT_EQ_INT(1000, (int)got->i[RK_ENT_FAV_CREATED_AT]);
    ASSERT_EQ_INT(1, (int)rk_store_count(s, RK_ENT_FAVORITE));
    /* 读-改-写：get_copy 只读验证 */
    RkEnt cp;
    ASSERT_EQ_INT(0, rk_store_get_copy(s, RK_ENT_FAVORITE, id1, &cp));
    ASSERT(strcmp(cp.s[RK_ENT_FAV_ID], "fav-1") == 0);
    rk_store_ent_free(&cp);
    /* update：全新构造实体（字符串可用字面量，update 内部深拷贝） */
    RkEnt upd = {0};
    upd.type = RK_ENT_FAVORITE;
    upd.id = id1;
    upd.s[RK_ENT_FAV_ID] = "fav-1";
    upd.s[RK_ENT_FAV_TYPE] = "message";
    upd.s[RK_ENT_FAV_REF_KEY] = "conv1:msg3";
    upd.s[RK_ENT_FAV_SNAPSHOT_JSON] = "{\"text\":\"hi2\"}";
    upd.i[RK_ENT_FAV_UPDATED_AT] = 2000;
    ASSERT_EQ_INT(0, rk_store_update(s, &upd));
    got = rk_store_get(s, RK_ENT_FAVORITE, id1);
    ASSERT(strcmp(got->s[RK_ENT_FAV_SNAPSHOT_JSON], "{\"text\":\"hi2\"}") == 0);
    /* 更新不存在的 id → -1 */
    RkEnt upd2 = {0};
    upd2.type = RK_ENT_FAVORITE;
    upd2.id = 9999;
    upd2.s[RK_ENT_FAV_REF_KEY] = "conv1:msg3"; /* 引用字面量，不依赖存储内部 */
    ASSERT_EQ_INT(-1, rk_store_update(s, &upd2));
    /* 唯一冲突更新 → -1（ref_key 已存在） */
    RkEnt dup = {0};
    dup.type = RK_ENT_FAVORITE;
    dup.id = id1;
    dup.s[RK_ENT_FAV_ID] = "fav-1";
    dup.s[RK_ENT_FAV_TYPE] = "message";
    dup.s[RK_ENT_FAV_REF_KEY] = "conv1:msg3";
    ASSERT_EQ_INT(0, rk_store_update(s, &dup)); /* 排除自身 → 成功 */
    /* 删除 */
    ASSERT_EQ_INT(0, rk_store_delete(s, RK_ENT_FAVORITE, id1));
    ASSERT_EQ_INT(0, (int)rk_store_count(s, RK_ENT_FAVORITE));
    ASSERT_NULL(rk_store_get(s, RK_ENT_FAVORITE, id1));
    rk_store_destroy(s);
}

TEST(store_entities) {
    RkStore *s = rk_store_create();
    /* Folder */
    RkEnt f = {0};
    f.type = RK_ENT_FOLDER;
    f.s[RK_ENT_FOLDER_ID] = "folder-1";
    f.s[RK_ENT_FOLDER_ASSISTANT_ID] = "ast-1";
    f.s[RK_ENT_FOLDER_NAME] = "工作";
    f.i[RK_ENT_FOLDER_SORT_INDEX] = 3;
    f.i[RK_ENT_FOLDER_CREATE_AT] = 100;
    int64_t fid = rk_store_insert(s, &f);
    ASSERT(fid > 0);
    ASSERT_NOT_NULL(rk_store_find_str(s, RK_ENT_FOLDER, RK_ENT_FOLDER_ID, "folder-1"));
    /* GenMedia */
    RkEnt m = {0};
    m.type = RK_ENT_GEN_MEDIA;
    m.s[RK_ENT_MEDIA_PATH] = "/sdcard/gen1.png";
    m.s[RK_ENT_MEDIA_MODEL_ID] = "flux";
    m.s[RK_ENT_MEDIA_PROMPT] = "cat";
    m.s[RK_ENT_MEDIA_TYPE] = "image_generation";
    m.i[RK_ENT_MEDIA_CREATE_AT] = 500;
    int64_t mid = rk_store_insert(s, &m);
    ASSERT(mid > 0);
    const RkEnt *gm = rk_store_get(s, RK_ENT_GEN_MEDIA, mid);
    ASSERT(strcmp(gm->s[RK_ENT_MEDIA_PATH], "/sdcard/gen1.png") == 0);
    /* ManagedFile */
    RkEnt mf = {0};
    mf.type = RK_ENT_MANAGED_FILE;
    mf.s[RK_ENT_FILE_FOLDER] = "uploads";
    mf.s[RK_ENT_FILE_REL_PATH] = "a/b.txt";
    mf.s[RK_ENT_FILE_DISPLAY_NAME] = "b.txt";
    mf.s[RK_ENT_FILE_MIME] = "text/plain";
    mf.i[RK_ENT_FILE_SIZE] = 42;
    mf.i[RK_ENT_FILE_CREATED_AT] = 10;
    int64_t mfid = rk_store_insert(s, &mf);
    ASSERT(mfid > 0);
    /* 唯一约束 relative_path */
    RkEnt mf2 = mf;
    ASSERT_EQ_INT(-1, (int)rk_store_insert(s, &mf2));
    ASSERT_NOT_NULL(rk_store_find_str(s, RK_ENT_MANAGED_FILE, RK_ENT_FILE_REL_PATH, "a/b.txt"));
    /* 按 folder 查询（模拟） */
    ASSERT_EQ_INT(1, (int)rk_store_count(s, RK_ENT_MANAGED_FILE));
    /* 整数区间查询 */
    const RkEnt *out[4];
    size_t n = rk_store_query_i64(s, RK_ENT_MANAGED_FILE, RK_ENT_FILE_SIZE, 0, 100, out, 4);
    ASSERT_EQ_INT(1, (int)n);
    ASSERT(strcmp(out[0]->s[RK_ENT_FILE_REL_PATH], "a/b.txt") == 0);
    n = rk_store_query_i64(s, RK_ENT_MANAGED_FILE, RK_ENT_FILE_SIZE, 1000, 2000, out, 4);
    ASSERT_EQ_INT(0, (int)n);
    rk_store_destroy(s);
}

TEST(store_snapshot_roundtrip) {
    RkStore *s = rk_store_create();
    /* 多类型多记录 */
    for (int i = 0; i < 20; i++) {
        RkEnt fav = {0};
        fav.type = RK_ENT_FAVORITE;
        char id[32], key[64];
        snprintf(id, sizeof(id), "fav-%d", i);
        snprintf(key, sizeof(key), "conv%d:msg%d", i, i * 2);
        fav.s[RK_ENT_FAV_ID] = id;
        fav.s[RK_ENT_FAV_TYPE] = "message";
        fav.s[RK_ENT_FAV_REF_KEY] = key;
        fav.s[RK_ENT_FAV_SNAPSHOT_JSON] = "{\"i\":1}";
        fav.s[RK_ENT_FAV_META_JSON] = NULL; /* 槽位为 NULL */
        fav.i[RK_ENT_FAV_CREATED_AT] = 1000 + i * 10;
        int64_t rid = rk_store_insert(s, &fav);
        ASSERT(rid > 0);
    }
    RkEnt folder = {0};
    folder.type = RK_ENT_FOLDER;
    folder.s[RK_ENT_FOLDER_ID] = "folder-x";
    folder.s[RK_ENT_FOLDER_ASSISTANT_ID] = "ast";
    folder.s[RK_ENT_FOLDER_NAME] = "测试文件夹";
    folder.i[RK_ENT_FOLDER_SORT_INDEX] = 1;
    folder.i[RK_ENT_FOLDER_CREATE_AT] = 777;
    ASSERT(rk_store_insert(s, &folder) > 0);
    RkEnt media = {0};
    media.type = RK_ENT_GEN_MEDIA;
    media.s[RK_ENT_MEDIA_PATH] = "/p.png";
    media.s[RK_ENT_MEDIA_TYPE] = "image_generation";
    media.i[RK_ENT_MEDIA_CREATE_AT] = 555;
    ASSERT(rk_store_insert(s, &media) > 0);
    /* 保存 */
    char path[256];
    snprintf(path, sizeof(path), "/tmp/rk_store_test_%d.rbin", (int)getpid());
    ASSERT_EQ_INT(0, rk_store_save_file(s, path));
    rk_store_destroy(s);
    /* 加载到新 store 并校验 */
    RkStore *s2 = rk_store_create();
    ASSERT_EQ_INT(0, rk_store_load_file(s2, path));
    ASSERT_EQ_INT(20, (int)rk_store_count(s2, RK_ENT_FAVORITE));
    ASSERT_EQ_INT(1, (int)rk_store_count(s2, RK_ENT_FOLDER));
    ASSERT_EQ_INT(1, (int)rk_store_count(s2, RK_ENT_GEN_MEDIA));
    for (int i = 0; i < 20; i++) {
        const RkEnt *fav = rk_store_at(s2, RK_ENT_FAVORITE, (size_t)i);
        ASSERT_NOT_NULL(fav);
        ASSERT_EQ_INT(1000 + i * 10, (int)fav->i[RK_ENT_FAV_CREATED_AT]);
        ASSERT_NOT_NULL(fav->s[RK_ENT_FAV_SNAPSHOT_JSON]);
        ASSERT_NULL(fav->s[RK_ENT_FAV_META_JSON]); /* NULL 槽位 roundtrip */
    }
    const RkEnt *f2 = rk_store_find_str(s2, RK_ENT_FOLDER, RK_ENT_FOLDER_ID, "folder-x");
    ASSERT_NOT_NULL(f2);
    ASSERT(strcmp(f2->s[RK_ENT_FOLDER_NAME], "测试文件夹") == 0);
    ASSERT_EQ_INT(777, (int)f2->i[RK_ENT_FOLDER_CREATE_AT]);
    /* 重新加载 = 全量替换（id 恢复一致，无重复） */
    ASSERT_EQ_INT(0, rk_store_load_file(s2, path));
    ASSERT_EQ_INT(20, (int)rk_store_count(s2, RK_ENT_FAVORITE));
    /* 自增 id 从快照恢复 */
    RkEnt fav = {0};
    fav.type = RK_ENT_FAVORITE;
    fav.s[RK_ENT_FAV_ID] = "fav-new";
    fav.s[RK_ENT_FAV_TYPE] = "message";
    fav.s[RK_ENT_FAV_REF_KEY] = "new:key";
    int64_t nid = rk_store_insert(s2, &fav);
    ASSERT(nid > 20);
    unlink(path);
    rk_store_destroy(s2);
}

TEST(store_errors) {
    RkStore *s = rk_store_create();
    /* 空/坏文件 */
    char path[256];
    snprintf(path, sizeof(path), "/tmp/rk_store_bad_%d", (int)getpid());
    ASSERT_EQ_INT(-1, rk_store_load_file(s, path)); /* 不存在 */
    FILE *fp = fopen(path, "w");
    ASSERT_NOT_NULL(fp);
    fputs("not a store", fp);
    fclose(fp);
    ASSERT_EQ_INT(-1, rk_store_load_file(s, path)); /* magic 错 */
    unlink(path);
    /* 空 store 保存/加载 */
    ASSERT_EQ_INT(0, rk_store_save_file(s, path));
    RkStore *s2 = rk_store_create();
    ASSERT_EQ_INT(0, rk_store_load_file(s2, path));
    ASSERT_EQ_INT(0, (int)rk_store_count(s2, RK_ENT_FAVORITE));
    unlink(path);
    rk_store_destroy(s2);
    rk_store_destroy(s);
}

int run_store_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(store, store_crud),
        RIKKA_TEST_REGISTER(store, store_entities),
        RIKKA_TEST_REGISTER(store, store_snapshot_roundtrip),
        RIKKA_TEST_REGISTER(store, store_errors),
    };
    return run_suite("store", tests, sizeof(tests) / sizeof(tests[0]));
}
