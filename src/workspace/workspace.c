#define _POSIX_C_SOURCE 200809L
#include "rikka/workspace/workspace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>

int rk_workspace_init(RkWorkspace *w, const char *root) {
    if (!w || !root) return -1;
    /* 规范化 root */
    char resolved[PATH_MAX];
    if (!realpath(root, resolved)) return -1;
    size_t len = strlen(resolved);
    if (len >= sizeof(w->root)) return -1;
    memcpy(w->root, resolved, len + 1);
    return 0;
}

int rk_workspace_safe_path(RkWorkspace *w, const char *path, char *out, size_t cap) {
    if (!w || !path || !out) return -1;
    /* 构建完整路径 */
    char full[PATH_MAX];
    if (path[0] == '/') {
        /* 绝对路径：检查是否在 root 内 */
        if (strncmp(path, w->root, strlen(w->root)) != 0) return -1;
        if (strlen(path) >= sizeof(full)) return -1;
        strcpy(full, path);
    } else {
        /* 相对路径：拼接 root */
        int n = snprintf(full, sizeof(full), "%s/%s", w->root, path);
        if (n < 0 || (size_t)n >= sizeof(full)) return -1;
    }
    /* 规范化 */
    char resolved[PATH_MAX];
    if (!realpath(full, resolved)) {
        /* 文件不存在：检查父目录 */
        char parent[PATH_MAX];
        strcpy(parent, full);
        char *slash = strrchr(parent, '/');
        if (slash) {
            *slash = '\0';
            if (!realpath(parent, resolved)) return -1;
            /* 重新拼接文件名 */
            strcat(resolved, "/");
            strcat(resolved, slash + 1);
        } else {
            return -1;
        }
    }
    /* 检查是否在 root 内 */
    size_t root_len = strlen(w->root);
    if (strncmp(resolved, w->root, root_len) != 0) return -1;
    if (resolved[root_len] != '/' && resolved[root_len] != '\0') return -1;
    if (strlen(resolved) >= cap) return -1;
    strcpy(out, resolved);
    return 0;
}

int rk_workspace_read(RkWorkspace *w, const char *path, char **out, size_t *len) {
    char safe[PATH_MAX];
    if (rk_workspace_safe_path(w, path, safe, sizeof(safe)) != 0) return -1;
    FILE *f = fopen(safe, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return -1; }
    char *data = (char *)malloc((size_t)sz + 1);
    if (!data) { fclose(f); return -1; }
    size_t n = fread(data, 1, (size_t)sz, f);
    fclose(f);
    data[n] = '\0';
    *out = data;
    *len = n;
    return 0;
}

int rk_workspace_write(RkWorkspace *w, const char *path, const char *data, size_t len) {
    char safe[PATH_MAX];
    if (rk_workspace_safe_path(w, path, safe, sizeof(safe)) != 0) return -1;
    FILE *f = fopen(safe, "wb");
    if (!f) return -1;
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len ? 0 : -1;
}

int rk_workspace_list(RkWorkspace *w, const char *path, char ***entries, size_t *count) {
    char safe[PATH_MAX];
    if (rk_workspace_safe_path(w, path, safe, sizeof(safe)) != 0) return -1;
    DIR *d = opendir(safe);
    if (!d) return -1;
    char **list = NULL;
    size_t n = 0, cap = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 16;
            char **nl = (char **)realloc(list, nc * sizeof(char *));
            if (!nl) break;
            list = nl;
            cap = nc;
        }
        list[n] = strdup(ent->d_name);
        if (list[n]) n++;
    }
    closedir(d);
    *entries = list;
    *count = n;
    return 0;
}

void rk_workspace_list_free(char **entries, size_t count) {
    if (!entries) return;
    for (size_t i = 0; i < count; i++) free(entries[i]);
    free(entries);
}
