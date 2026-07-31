#ifndef RIKKA_WORKSPACE_WORKSPACE_H
#define RIKKA_WORKSPACE_WORKSPACE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Workspace 直 IO：直接文件系统操作（无 proot 沙箱）。
 * 路径穿越防护：规范化路径，检查是否在 root 内。
 */

typedef struct {
    char root[1024];  /* workspace 根目录（规范化） */
} RkWorkspace;

/* 初始化 workspace（root 规范化） */
int rk_workspace_init(RkWorkspace *w, const char *root);

/* 读文件（返回 malloc 数据，调用方 free） */
int rk_workspace_read(RkWorkspace *w, const char *path, char **out, size_t *len);

/* 写文件 */
int rk_workspace_write(RkWorkspace *w, const char *path, const char *data, size_t len);

/* 列目录（返回 malloc 文件名数组，调用方 rk_workspace_list_free） */
int rk_workspace_list(RkWorkspace *w, const char *path, char ***entries, size_t *count);
void rk_workspace_list_free(char **entries, size_t count);

/* 路径安全检查：规范化后是否在 root 内 */
int rk_workspace_safe_path(RkWorkspace *w, const char *path, char *out, size_t cap);

#endif /* RIKKA_WORKSPACE_WORKSPACE_H */
