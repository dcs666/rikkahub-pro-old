/*
 * 工具系统实现（见 tool.h）。
 *
 * 内置工具：
 *  - get_time_info：本地时间/时区信息（对齐 JVM TimeInfoTool）
 *  - workspace_read_file / workspace_write_file / workspace_edit_file：
 *    文件读写编辑（路径解析到 workspace_root 内，防 ../ 逃逸）
 *  - workspace_shell：沙箱 shell（fork + pipe + timeout + cwd）
 *  - memory_tool：长期记忆 create/edit/delete（env 回调）
 *  - use_skill：加载技能 SKILL.md（/skills/<name>/，路径安全）
 */
#define _POSIX_C_SOURCE 200809L
#include "rikka/ai/tool.h"
#include "rikka/core/buffer.h"
#include "rikka/json/json.h"
#include "rikka/util/arena.h"
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ================= 注册表 ================= */

void rk_tools_init(RkToolRegistry *r) {
    r->tools = NULL;
    r->count = r->cap = 0;
}

void rk_tools_destroy(RkToolRegistry *r) {
    free(r->tools);
    r->tools = NULL;
    r->count = r->cap = 0;
}

int rk_tools_add(RkToolRegistry *r, const RkTool *t) {
    if (!r || !t || !t->name) return -1;
    if (rk_tools_find(r, t->name)) return -1; /* 重名拒绝 */
    if (r->count == r->cap) {
        size_t nc = r->cap ? r->cap * 2 : 8;
        const RkTool **nt = (const RkTool **)realloc(r->tools, nc * sizeof(*nt));
        if (!nt) return -1;
        r->tools = nt;
        r->cap = nc;
    }
    r->tools[r->count++] = t;
    return 0;
}

const RkTool *rk_tools_find(const RkToolRegistry *r, const char *name) {
    if (!r || !name) return NULL;
    for (size_t i = 0; i < r->count; i++) {
        if (strcmp(r->tools[i]->name, name) == 0) return r->tools[i];
    }
    return NULL;
}

size_t rk_tools_count(const RkToolRegistry *r) { return r ? r->count : 0; }

const RkTool *rk_tools_at(const RkToolRegistry *r, size_t idx) {
    if (!r || idx >= r->count) return NULL;
    return r->tools[idx];
}

int rk_tool_call(const RkTool *t, const char *args_json, const RkToolEnv *env,
                 char **result) {
    if (!t || !t->call) return -1;
    return t->call(t, args_json ? args_json : "{}", env, result);
}

/* ================= JSON 参数辅助 ================= */

int rk_tool_arg_str(const char *args_json, const char *key, char *out, size_t out_sz) {
    if (!args_json || !key || !out || out_sz == 0) return -1;
    Arena *a = arena_create(0);
    size_t err = 0;
    RJson *v = rjson_parse(a, args_json, strlen(args_json), &err);
    int rc = -1;
    if (v) {
        const RJson *val = rjson_obj_get(v, key);
        if (val && val->type == RJSON_STRING && val->u.str.len < out_sz) {
            memcpy(out, val->u.str.ptr, val->u.str.len);
            out[val->u.str.len] = '\0';
            rc = 0;
        }
    }
    arena_destroy(a);
    return rc;
}

int64_t rk_tool_arg_i64(const char *args_json, const char *key, int64_t dflt) {
    Arena *a = arena_create(0);
    size_t err = 0;
    RJson *v = rjson_parse(a, args_json, strlen(args_json), &err);
    int64_t rc = dflt;
    if (v) {
        const RJson *val = rjson_obj_get(v, key);
        if (val && val->type == RJSON_NUMBER) rc = (int64_t)val->u.number;
    }
    arena_destroy(a);
    return rc;
}

int rk_tool_arg_bool(const char *args_json, const char *key, int dflt) {
    Arena *a = arena_create(0);
    size_t err = 0;
    RJson *v = rjson_parse(a, args_json, strlen(args_json), &err);
    int rc = dflt;
    if (v) {
        const RJson *val = rjson_obj_get(v, key);
        if (val && val->type == RJSON_BOOL) rc = val->u.boolean;
    }
    arena_destroy(a);
    return rc;
}

char *rk_tool_result_json(const char *key, const char *value) {
    size_t cap = strlen(key) + strlen(value) + 16;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    snprintf(out, cap, "{\"%s\":\"%s\"}", key, value);
    return out;
}

char *rk_tool_result_error(const char *message) {
    size_t cap = strlen(message) + 16;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    snprintf(out, cap, "{\"error\":\"%s\"}", message);
    return out;
}

/* ================= 1. get_time_info ================= */

static int tool_time_info(const RkTool *t, const char *args_json, const RkToolEnv *env,
                          char **result) {
    (void)t;
    (void)args_json;
    (void)env;
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char tz[64] = "UTC";
    char offset[16] = "+00:00";
    {
        char tmp[64];
        struct tm tmv_utc;
        gmtime_r(&now, &tmv_utc);
        strftime(tmp, sizeof(tmp), "%Z", &tmv);
        if (tmp[0] && strcmp(tmp, "GMT") != 0) snprintf(tz, sizeof(tz), "%s", tmp);
        int off_min = (int)difftime(mktime(&tmv), mktime(&tmv_utc)) / 60;
        snprintf(offset, sizeof(offset), "%+03d:%02d", off_min / 60, off_min % 60);
    }
    char weekday[16], weekday_en[16], date[16], timebuf[16], datetime[32];
    static const char *WD[7] = {"Monday", "Tuesday", "Wednesday", "Thursday",
                                "Friday", "Saturday", "Sunday"};
    strftime(date, sizeof(date), "%Y-%m-%d", &tmv);
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", &tmv);
    snprintf(datetime, sizeof(datetime), "%sT%s", date, timebuf);
    snprintf(weekday, sizeof(weekday), "%s", WD[tmv.tm_wday == 0 ? 6 : tmv.tm_wday - 1]);
    snprintf(weekday_en, sizeof(weekday_en), "%s", weekday);
    size_t cap = 512;
    char *out = (char *)malloc(cap);
    if (!out) return -1;
    int n = snprintf(out, cap,
                     "{\"year\":%d,\"month\":%d,\"day\":%d,\"weekday\":\"%s\","
                     "\"weekday_en\":\"%s\",\"weekday_index\":%d,\"date\":\"%s\","
                     "\"time\":\"%s\",\"datetime\":\"%s\",\"timezone\":\"%s\","
                     "\"utc_offset\":\"%s\",\"timestamp_ms\":%lld}",
                     tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, weekday,
                     weekday_en, (tmv.tm_wday == 0 ? 7 : tmv.tm_wday), date,
                     timebuf, datetime, tz, offset, (long long)now * 1000);
    if (n <= 0 || (size_t)n >= cap) { free(out); return -1; }
    *result = out;
    return 0;
}

static const RkTool TOOL_TIME_INFO = {
    "get_time_info",
    "Get the current local date and time info from the device. "
    "Returns year/month/day, weekday, ISO date/time strings, timezone, and timestamp.",
    "{\"type\":\"object\",\"properties\":{}}",
    tool_time_info,
};

/* ================= workspace 路径安全 ================= */

/* 把用户路径解析到 root 内：拒绝绝对路径与 ../ 逃逸。
 * 返回 malloc 的完整路径（调用方 free）；非法返回 NULL。 */
static char *resolve_path(const char *root, const char *user_path) {
    if (!root || !user_path) return NULL;
    if (user_path[0] == '/') return NULL; /* 必须相对 workspace */
    /* 逐段检查 .. 逃逸 */
    const char *p = user_path;
    while (*p) {
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) return NULL;
        if (p[0] == '.' && (p[1] == '/' || p[1] == '\0')) return NULL;
        const char *slash = strchr(p, '/');
        if (!slash) break;
        p = slash + 1;
    }
    size_t rl = strlen(root);
    size_t pl = strlen(user_path);
    char *full = (char *)malloc(rl + 1 + pl + 1);
    if (!full) return NULL;
    memcpy(full, root, rl);
    full[rl] = '/';
    memcpy(full + rl + 1, user_path, pl + 1);
    return full;
}

static int tool_ws_read(const RkTool *t, const char *args_json, const RkToolEnv *env,
                        char **result) {
    (void)t;
    char path[1024];
    if (!env || !env->workspace_root ||
        rk_tool_arg_str(args_json, "path", path, sizeof(path)) != 0) {
        if (result) *result = rk_tool_result_error("path is required");
        return -1;
    }
    char *full = resolve_path(env->workspace_root, path);
    if (!full) {
        if (result) *result = rk_tool_result_error("invalid path");
        return -1;
    }
    int fd = open(full, O_RDONLY);
    free(full);
    if (fd < 0) {
        if (result) *result = rk_tool_result_error("file not found");
        return -1;
    }
    Buf content;
    buf_init(&content);
    char buf[8192];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r <= 0) break;
        buf_append(&content, buf, (size_t)r);
    }
    close(fd);
    /* 结果 JSON：{"content":"..."}（简单转义） */
    size_t cap = content.len * 2 + 32;
    char *out = (char *)malloc(cap);
    if (!out) { buf_free(&content); return -1; }
    size_t oi = 0;
    out[oi++] = '{';
    memcpy(out + oi, "\"content\":\"", 11);
    oi += 11;
    for (size_t i = 0; i < content.len && oi + 2 < cap; i++) {
        unsigned char c = (unsigned char)content.data[i];
        if (c == '"' || c == '\\') {
            out[oi++] = '\\';
            out[oi++] = (char)c;
        } else if (c == '\n') {
            memcpy(out + oi, "\\n", 2);
            oi += 2;
        } else if (c == '\r') {
            memcpy(out + oi, "\\r", 2);
            oi += 2;
        } else if (c == '\t') {
            memcpy(out + oi, "\\t", 2);
            oi += 2;
        } else if (c < 0x20) {
            oi += (size_t)snprintf(out + oi, cap - oi, "\\u%04x", c);
        } else {
            out[oi++] = (char)c;
        }
    }
    memcpy(out + oi, "\"}", 2);
    oi += 2;
    buf_free(&content);
    *result = out;
    return 0;
}

static int tool_ws_write(const RkTool *t, const char *args_json, const RkToolEnv *env,
                         char **result) {
    (void)t;
    char path[1024], text[65536];
    if (!env || !env->workspace_root ||
        rk_tool_arg_str(args_json, "path", path, sizeof(path)) != 0 ||
        rk_tool_arg_str(args_json, "text", text, sizeof(text)) != 0) {
        if (result) *result = rk_tool_result_error("path and text are required");
        return -1;
    }
    char *full = resolve_path(env->workspace_root, path);
    if (!full) {
        if (result) *result = rk_tool_result_error("invalid path");
        return -1;
    }
    /* 创建父目录 */
    char *slash = strrchr(full, '/');
    if (slash && slash != full) {
        *slash = '\0';
        /* 逐级 mkdir */
        char *p = full + 1;
        while (*p) {
            char *s = strchr(p, '/');
            if (s) *s = '\0';
            mkdir(full, 0755);
            if (s) { *s = '/'; p = s + 1; } else break;
        }
        *slash = '/';
    }
    int fd = open(full, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    free(full);
    if (fd < 0) {
        if (result) *result = rk_tool_result_error("cannot write file");
        return -1;
    }
    size_t len = strlen(text);
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, text + off, len - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    close(fd);
    if (result) *result = strdup("{\"ok\":true}");
    return 0;
}

static int tool_ws_edit(const RkTool *t, const char *args_json, const RkToolEnv *env,
                        char **result) {
    (void)t;
    char path[1024], old_text[4096], new_text[4096];
    if (!env || !env->workspace_root ||
        rk_tool_arg_str(args_json, "path", path, sizeof(path)) != 0 ||
        rk_tool_arg_str(args_json, "old_text", old_text, sizeof(old_text)) != 0 ||
        rk_tool_arg_str(args_json, "new_text", new_text, sizeof(new_text)) != 0) {
        if (result) *result = rk_tool_result_error("path, old_text and new_text are required");
        return -1;
    }
    int replace_all = rk_tool_arg_bool(args_json, "replace_all", 0);
    char *full = resolve_path(env->workspace_root, path);
    if (!full) {
        if (result) *result = rk_tool_result_error("invalid path");
        return -1;
    }
    /* 读原文件 */
    int fd = open(full, O_RDONLY);
    if (fd < 0) {
        free(full);
        if (result) *result = rk_tool_result_error("file not found");
        return -1;
    }
    Buf content;
    buf_init(&content);
    char buf[8192];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r <= 0) break;
        buf_append(&content, buf, (size_t)r);
    }
    close(fd);
    /* 三级替换（exact → line_trimmed → block_anchor，对标 JVM TextReplacers） */
    RkTextReplaceResult tr;
    rk_text_replace((const char *)content.data, content.len, old_text, new_text,
                    replace_all, &tr);
    buf_free(&content);
    if (tr.error != 0) {
        if (result) *result = rk_tool_result_error(tr.errmsg);
        free(tr.updated);
        free(full);
        return -1;
    }
    /* 写回（先写临时文件再 rename，原子替换） */
    char *tmp_full = resolve_path(env->workspace_root, ".edit_tmp");
    if (!tmp_full) { free(tr.updated); return -1; }
    int wfd = open(tmp_full, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (wfd < 0) { free(tmp_full); free(tr.updated); return -1; }
    size_t ulen = strlen(tr.updated);
    size_t off = 0;
    while (off < ulen) {
        ssize_t w = write(wfd, tr.updated + off, ulen - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    close(wfd);
    rename(tmp_full, full);
    free(tmp_full);
    free(full);
    if (result) {
        char msg[96];
        int mn = snprintf(msg, sizeof(msg), "{\"ok\":true,\"count\":%zu,\"strategy\":\"%s\"}",
                          tr.replacements, tr.strategy ? tr.strategy : "exact");
        if (mn > 0 && (size_t)mn < sizeof(msg)) {
            *result = strdup(msg); /* malloc，调用方 free */
        }
    }
    free(tr.updated);
    return 0;
}

/* fork + pipe + timeout 执行 shell 命令 */
static int run_shell_cmd(const char *cmd, const char *cwd, int timeout_s,
                         char **out, int *exit_code) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        /* 子进程 */
        dup2(pipefd[1], 1);
        dup2(pipefd[1], 2);
        close(pipefd[0]);
        close(pipefd[1]);
        if (cwd && cwd[0]) {
            int crc = chdir(cwd);
            (void)crc;
        }
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    /* 读输出（timeout 控制） */
    Buf outb;
    buf_init(&outb);
    char buf[4096];
    int timed_out = 0;
    int fd = pipefd[0];
    for (;;) {
        struct pollfd pf = {fd, POLLIN, 0};
        int pr = poll(&pf, 1, timeout_s > 0 ? timeout_s * 1000 : 30000);
        if (pr <= 0) {
            if (pr == 0) timed_out = 1;
            break;
        }
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r <= 0) break;
        buf_append(&outb, buf, (size_t)r);
    }
    close(fd);
    if (timed_out) {
        kill(pid, SIGKILL);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (exit_code) *exit_code = code;
    char *res = (char *)malloc(outb.len + 1);
    if (res) {
        if (outb.len > 0) memcpy(res, outb.data, outb.len); /* 空 Buf 保护（UBSan） */
        res[outb.len] = '\0';
    }
    buf_free(&outb);
    if (!res) return -1;
    *out = res;
    return timed_out ? -1 : 0;
}

static int tool_ws_shell(const RkTool *t, const char *args_json, const RkToolEnv *env,
                         char **result) {
    (void)t;
    char cmd[16384];
    if (rk_tool_arg_str(args_json, "command", cmd, sizeof(cmd)) != 0) {
        if (result) *result = rk_tool_result_error("command is required");
        return -1;
    }
    char *out = NULL;
    int code = 0;
    int timeout_s = (int)rk_tool_arg_i64(args_json, "timeout", 30);
    if (timeout_s < 1) timeout_s = 30;
    if (timeout_s > 600) timeout_s = 600;
    int rc = run_shell_cmd(cmd, env ? env->workspace_cwd : NULL, timeout_s, &out, &code);
    if (rc != 0) {
        free(out);
        if (result) *result = rk_tool_result_error("command timed out or failed");
        return -1;
    }
    /* 结果 JSON：{"output":"...","exit_code":N} */
    size_t olen = out ? strlen(out) : 0;
    size_t cap = olen * 2 + 64;
    char *json = (char *)malloc(cap);
    if (!json) { free(out); return -1; }
    size_t oi = 0;
    memcpy(json + oi, "{\"output\":\"", 11);
    oi += 11;
    for (size_t i = 0; i < olen && oi + 2 < cap; i++) {
        unsigned char c = (unsigned char)out[i];
        if (c == '"' || c == '\\') {
            json[oi++] = '\\';
            json[oi++] = (char)c;
        } else if (c == '\n') {
            memcpy(json + oi, "\\n", 2);
            oi += 2;
        } else if (c == '\r') {
            memcpy(json + oi, "\\r", 2);
            oi += 2;
        } else if (c == '\t') {
            memcpy(json + oi, "\\t", 2);
            oi += 2;
        } else if (c < 0x20) {
            oi += (size_t)snprintf(json + oi, cap - oi, "\\u%04x", c);
        } else {
            json[oi++] = (char)c;
        }
    }
    int n = snprintf(json + oi, cap - oi, "\",\"exit_code\":%d}", code);
    if (n <= 0 || oi + (size_t)n >= cap) { free(json); free(out); return -1; }
    free(out);
    *result = json;
    return 0;
}

/* ================= memory_tool ================= */

static int tool_memory(const RkTool *t, const char *args_json, const RkToolEnv *env,
                       char **result) {
    (void)t;
    char action[16], content[4096];
    if (rk_tool_arg_str(args_json, "action", action, sizeof(action)) != 0) {
        if (result) *result = rk_tool_result_error("action is required");
        return -1;
    }
    int64_t id = rk_tool_arg_i64(args_json, "id", 0);
    if (strcmp(action, "create") == 0) {
        if (!env || !env->memory_create) {
            if (result) *result = rk_tool_result_error("memory unavailable");
            return -1;
        }
        if (rk_tool_arg_str(args_json, "content", content, sizeof(content)) != 0) {
            if (result) *result = rk_tool_result_error("content is required");
            return -1;
        }
        char *r = env->memory_create(content, env->ud);
        if (!r) { if (result) *result = rk_tool_result_error("create failed"); return -1; }
        *result = r;
        return 0;
    }
    if (strcmp(action, "edit") == 0) {
        if (!env || !env->memory_edit) {
            if (result) *result = rk_tool_result_error("memory unavailable");
            return -1;
        }
        if (id <= 0 || rk_tool_arg_str(args_json, "content", content, sizeof(content)) != 0) {
            if (result) *result = rk_tool_result_error("id and content are required");
            return -1;
        }
        char *r = env->memory_edit(id, content, env->ud);
        if (!r) { if (result) *result = rk_tool_result_error("edit failed"); return -1; }
        *result = r;
        return 0;
    }
    if (strcmp(action, "delete") == 0) {
        if (!env || !env->memory_delete) {
            if (result) *result = rk_tool_result_error("memory unavailable");
            return -1;
        }
        if (id <= 0) {
            if (result) *result = rk_tool_result_error("id is required");
            return -1;
        }
        if (env->memory_delete(id, env->ud) != 0) {
            if (result) *result = rk_tool_result_error("delete failed");
            return -1;
        }
        if (result) *result = strdup("{\"ok\":true}");
        return 0;
    }
    if (result) *result = rk_tool_result_error("unknown action");
    return -1;
}

/* ================= use_skill ================= */

static int tool_use_skill(const RkTool *t, const char *args_json, const RkToolEnv *env,
                          char **result) {
    (void)t;
    (void)env;
    char name[128], relbuf[512];
    if (rk_tool_arg_str(args_json, "name", name, sizeof(name)) != 0) {
        if (result) *result = rk_tool_result_error("name is required");
        return -1;
    }
    /* name 不得含路径分隔符 */
    for (const char *p = name; *p; p++) {
        if (*p == '/' || *p == '\\' || *p == '.') {
            if (result) *result = rk_tool_result_error("invalid skill name");
            return -1;
        }
    }
    const char *rel = "SKILL.md";
    if (rk_tool_arg_str(args_json, "path", relbuf, sizeof(relbuf)) == 0) {
        rel = relbuf;
        for (const char *p = rel; *p; p++) {
            if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0')) {
                if (result) *result = rk_tool_result_error("invalid path");
                return -1;
            }
        }
    }
    /* /skills/<name>/<rel> */
    size_t cap = 8 + strlen(name) + 1 + strlen(rel) + 1;
    char *full = (char *)malloc(cap);
    if (!full) return -1;
    snprintf(full, cap, "/skills/%s/%s", name, rel);
    int fd = open(full, O_RDONLY);
    if (fd < 0) {
        free(full);
        if (result) *result = rk_tool_result_error("skill not found");
        return -1;
    }
    Buf content;
    buf_init(&content);
    char buf[8192];
    for (;;) {
        ssize_t r = read(fd, buf, sizeof(buf));
        if (r <= 0) break;
        buf_append(&content, buf, (size_t)r);
    }
    close(fd);
    free(full);
    /* 结果：{"content":"..."} */
    size_t rcap = content.len * 2 + 32;
    char *out = (char *)malloc(rcap);
    if (!out) { buf_free(&content); return -1; }
    size_t oi = 0;
    memcpy(out + oi, "{\"content\":\"", 12);
    oi += 12;
    for (size_t i = 0; i < content.len && oi + 2 < rcap; i++) {
        unsigned char c = (unsigned char)content.data[i];
        if (c == '"' || c == '\\') {
            out[oi++] = '\\';
            out[oi++] = (char)c;
        } else if (c == '\n') {
            memcpy(out + oi, "\\n", 2);
            oi += 2;
        } else if (c < 0x20) {
            oi += (size_t)snprintf(out + oi, rcap - oi, "\\u%04x", c);
        } else {
            out[oi++] = (char)c;
        }
    }
    memcpy(out + oi, "\"}", 2);
    oi += 2;
    buf_free(&content);
    *result = out;
    return 0;
}

/* ================= search_web / conversation 工具 ================= */

static int tool_search_web(const RkTool *t, const char *args_json, const RkToolEnv *env,
                           char **result) {
    (void)t;
    char query[2048];
    if (rk_tool_arg_str(args_json, "query", query, sizeof(query)) != 0) {
        if (result) *result = rk_tool_result_error("query is required");
        return -1;
    }
    if (!env || !env->web_search) {
        if (result) *result = rk_tool_result_error("web search unavailable");
        return -1;
    }
    char *r = env->web_search(query, env->ud);
    if (!r) {
        if (result) *result = rk_tool_result_error("search failed");
        return -1;
    }
    *result = r; /* 回调返回的 JSON 字符串（items[]/images[]） */
    return 0;
}

static int tool_recent_chats(const RkTool *t, const char *args_json, const RkToolEnv *env,
                             char **result) {
    (void)t;
    if (!env || !env->recent_chats) {
        if (result) *result = rk_tool_result_error("conversation store unavailable");
        return -1;
    }
    int64_t limit = rk_tool_arg_i64(args_json, "limit", 10);
    if (limit < 1) limit = 1;
    if (limit > 30) limit = 30;
    char *r = env->recent_chats((int)limit, env->ud);
    if (!r) {
        if (result) *result = rk_tool_result_error("query failed");
        return -1;
    }
    *result = r;
    return 0;
}

static int tool_conversation_search(const RkTool *t, const char *args_json,
                                    const RkToolEnv *env, char **result) {
    (void)t;
    char query[1024];
    if (rk_tool_arg_str(args_json, "query", query, sizeof(query)) != 0) {
        if (result) *result = rk_tool_result_error("query is required");
        return -1;
    }
    if (!env || !env->conversation_search) {
        if (result) *result = rk_tool_result_error("conversation store unavailable");
        return -1;
    }
    char *r = env->conversation_search(query, env->ud);
    if (!r) {
        if (result) *result = rk_tool_result_error("search failed");
        return -1;
    }
    *result = r;
    return 0;
}

static const RkTool TOOL_SEARCH_WEB = {
    "search_web",
    "Search the web for up-to-date or specific information. "
    "Use this when the user asks for the latest news, current facts, or needs verification. "
    "Generate focused keywords and run multiple searches if needed.",
    "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Search keywords\"}},\"required\":[\"query\"]}",
    tool_search_web,
};

static const RkTool TOOL_RECENT_CHATS = {
    "recent_chats",
    "List the user's recent conversations with you to understand their preferences and ongoing topics. "
    "Returns conversation titles and the date of last activity, ordered by pinned first then most recently updated.",
    "{\"type\":\"object\",\"properties\":{\"limit\":{\"type\":\"integer\",\"description\":\"Max conversations (default 10, max 30)\"}}}",
    tool_recent_chats,
};

static const RkTool TOOL_CONVERSATION_SEARCH = {
    "conversation_search",
    "Full-text search across the user's past conversations to recall specific information they mentioned before. "
    "Use focused keywords. Each result includes the conversation title, a snippet, and the date.",
    "{\"type\":\"object\",\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"Keywords to search for\"}},\"required\":[\"query\"]}",
    tool_conversation_search,
};

/* ================= 注册 ================= */

static const RkTool TOOL_WS_READ = {
    "workspace_read_file",
    "Read file contents from the workspace files area. Path is relative to the workspace root.",
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Relative path inside the workspace\"}},\"required\":[\"path\"]}",
    tool_ws_read,
};

static const RkTool TOOL_WS_WRITE = {
    "workspace_write_file",
    "Create or overwrite a file in the workspace. Creates parent directories as needed.",
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"text\":{\"type\":\"string\"}},\"required\":[\"path\",\"text\"]}",
    tool_ws_write,
};

static const RkTool TOOL_WS_EDIT = {
    "workspace_edit_file",
    "Make precise edits to an existing file. old_text must occur exactly once unless replace_all is set.",
    "{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},\"old_text\":{\"type\":\"string\"},\"new_text\":{\"type\":\"string\"},\"replace_all\":{\"type\":\"boolean\"}},\"required\":[\"path\",\"old_text\",\"new_text\"]}",
    tool_ws_edit,
};

static const RkTool TOOL_WS_SHELL = {
    "workspace_shell",
    "Run shell commands in the workspace sandbox. Returns combined stdout/stderr output and exit code.",
    "{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},\"timeout\":{\"type\":\"integer\",\"description\":\"Seconds (default 30, max 600)\"}},\"required\":[\"command\"]}",
    tool_ws_shell,
};

static const RkTool TOOL_MEMORY = {
    "memory_tool",
    "The memory tool stores long-term information across conversations. "
    "Use `action` to control the operation: `create` (add), `edit` (update), `delete` (remove).",
    "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"create\",\"edit\",\"delete\"]},\"id\":{\"type\":\"integer\"},\"content\":{\"type\":\"string\"}},\"required\":[\"action\"]}",
    tool_memory,
};

static const RkTool TOOL_USE_SKILL = {
    "use_skill",
    "Load and apply a skill to get specialized instructions. "
    "Call this tool when the user's request matches an available skill.",
    "{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},\"path\":{\"type\":\"string\",\"description\":\"Optional relative path inside the skill directory; omit for SKILL.md\"}},\"required\":[\"name\"]}",
    tool_use_skill,
};

void rk_tools_register_builtin(RkToolRegistry *r, const RkToolEnv *env) {
    rk_tools_add(r, &TOOL_TIME_INFO);
    if (env && env->workspace_root) {
        rk_tools_add(r, &TOOL_WS_READ);
        rk_tools_add(r, &TOOL_WS_WRITE);
        rk_tools_add(r, &TOOL_WS_EDIT);
        rk_tools_add(r, &TOOL_WS_SHELL);
    }
    rk_tools_add(r, &TOOL_MEMORY);
    rk_tools_add(r, &TOOL_USE_SKILL);
    if (env) {
        if (env->web_search) rk_tools_add(r, &TOOL_SEARCH_WEB);
        if (env->recent_chats) rk_tools_add(r, &TOOL_RECENT_CHATS);
        if (env->conversation_search) rk_tools_add(r, &TOOL_CONVERSATION_SEARCH);
    }
}
