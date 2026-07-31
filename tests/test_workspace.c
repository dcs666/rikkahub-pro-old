#include "test.h"
#include "rikka/workspace/workspace.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

TEST(workspace_read_write) {
    /* 创建测试目录 */
    mkdir("/tmp/test_ws", 0755);
    RkWorkspace w;
    ASSERT_EQ_INT(0, rk_workspace_init(&w, "/tmp/test_ws"));
    /* 写文件 */
    const char *data = "hello workspace";
    ASSERT_EQ_INT(0, rk_workspace_write(&w, "test.txt", data, strlen(data)));
    /* 读文件 */
    char *out = NULL;
    size_t len = 0;
    ASSERT_EQ_INT(0, rk_workspace_read(&w, "test.txt", &out, &len));
    ASSERT_EQ_SIZE(strlen(data), len);
    ASSERT(memcmp(out, data, len) == 0);
    free(out);
    /* 列目录 */
    char **entries = NULL;
    size_t count = 0;
    ASSERT_EQ_INT(0, rk_workspace_list(&w, ".", &entries, &count));
    ASSERT(count >= 1);
    int found = 0;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i], "test.txt") == 0) found = 1;
    }
    ASSERT(found);
    rk_workspace_list_free(entries, count);
    unlink("/tmp/test_ws/test.txt");
    rmdir("/tmp/test_ws");
}

TEST(workspace_path_traversal) {
    mkdir("/tmp/test_ws2", 0755);
    RkWorkspace w;
    ASSERT_EQ_INT(0, rk_workspace_init(&w, "/tmp/test_ws2"));
    /* 路径穿越攻击：../ 逃逸 */
    char *out = NULL;
    size_t len = 0;
    ASSERT(rk_workspace_read(&w, "../etc/passwd", &out, &len) != 0);
    ASSERT(rk_workspace_read(&w, "../../etc/passwd", &out, &len) != 0);
    /* 绝对路径逃逸 */
    ASSERT(rk_workspace_read(&w, "/etc/passwd", &out, &len) != 0);
    rmdir("/tmp/test_ws2");
}

int run_workspace_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(workspace, workspace_read_write),
        RIKKA_TEST_REGISTER(workspace, workspace_path_traversal),
    };
    return run_suite("workspace", tests, sizeof(tests) / sizeof(tests[0]));
}
