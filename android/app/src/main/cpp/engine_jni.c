/*
 * RikkaHub CE: 引擎 JNI 桥。
 * 暴露: nativeChat(providerJson, historyJson, callback) / nativeSetCancel(v)
 * 线程模型: nativeChat 阻塞调用方线程（Kotlin 侧放协程 IO 线程）;
 * 回调可能由引擎流水线线程(pipe_processor)触发 —— 统一经 jni_thread_env
 * (GetEnv 优先 + AttachCurrentThread + 调用后 Detach) 保证 JNIEnv 合法。
 */
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rikka/ai/chat.h"
#include "rikka/ai/ocr.h"
#include "rikka/ai/prompt.h"
#include "rikka/ai/tool.h"

/* 前向声明（parse_history 在 b64_encode 定义前调用） */
static size_t b64_encode(const uint8_t *in, size_t in_len, char *out);
#include "rikka/core/message.h"
#include "rikka/json/json.h"
#include "rikka/util/arena.h"

static volatile int g_cancel = 0;
static JavaVM *g_vm = NULL;

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    g_vm = vm;
    return JNI_VERSION_1_6;
}

/* ---------- 回调桥 ---------- */

typedef struct {
    JNIEnv *env;
    jobject cb; /* 全局引用 */
} JniCb;

static jmethodID g_m_delta, g_m_tool_call, g_m_tool_result, g_m_finish;

/* 获取当前线程 JNIEnv: Java 线程直接取; 引擎读/处理线程(异步流水线)先 attach。
 * 返回 1 表示本调用 attach 的(调用方负责 detach)。 */
static JNIEnv *jni_thread_env(int *attached) {
    JNIEnv *env = NULL;
    *attached = 0;
    if (!g_vm) return NULL;
    if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) == JNI_OK) return env;
    if ((*g_vm)->AttachCurrentThread(g_vm, &env, NULL) != JNI_OK) return NULL;
    *attached = 1;
    return env;
}

static void jni_delta(void *ud, int kind, const char *data, size_t len) {
    JniCb *jc = (JniCb *)ud;
    int attached = 0;
    JNIEnv *env = jni_thread_env(&attached);
    if (!env) return;
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env); /* 防上次异常污染 */
    /* NewStringUTF 需要 NUL 结尾：小块栈拷贝，大块堆拷贝（不截断防丢内容） */
    char tmp[1024];
    char *heap = NULL;
    const char *s;
    if (len < sizeof(tmp)) {
        memcpy(tmp, data, len);
        tmp[len] = '\0';
        s = tmp;
    } else {
        heap = (char *)malloc(len + 1);
        if (!heap) {
            if (attached) (*g_vm)->DetachCurrentThread(g_vm);
            return;
        }
        memcpy(heap, data, len);
        heap[len] = '\0';
        s = heap;
    }
    jstring js = (*env)->NewStringUTF(env, s);
    if (js) {
        (*env)->CallVoidMethod(env, jc->cb, g_m_delta, (jint)kind, js);
        (*env)->DeleteLocalRef(env, js);
    }
    free(heap);
    if (attached) (*g_vm)->DetachCurrentThread(g_vm);
}

static void jni_tool_call(void *ud, const char *name, const char *args) {
    JniCb *jc = (JniCb *)ud;
    int attached = 0;
    JNIEnv *env = jni_thread_env(&attached);
    if (!env) return;
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env); /* 防上次异常污染 */
    jstring n = (*env)->NewStringUTF(env, name);
    jstring a = (*env)->NewStringUTF(env, args);
    if (n && a) (*env)->CallVoidMethod(env, jc->cb, g_m_tool_call, n, a);
    if (n) (*env)->DeleteLocalRef(env, n);
    if (a) (*env)->DeleteLocalRef(env, a);
    if (attached) (*g_vm)->DetachCurrentThread(g_vm);
}

static void jni_tool_result(void *ud, const char *name, const char *result) {
    JniCb *jc = (JniCb *)ud;
    int attached = 0;
    JNIEnv *env = jni_thread_env(&attached);
    if (!env) return;
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env); /* 防上次异常污染 */
    jstring n = (*env)->NewStringUTF(env, name);
    jstring r = (*env)->NewStringUTF(env, result);
    if (n && r) (*env)->CallVoidMethod(env, jc->cb, g_m_tool_result, n, r);
    if (n) (*env)->DeleteLocalRef(env, n);
    if (r) (*env)->DeleteLocalRef(env, r);
    if (attached) (*g_vm)->DetachCurrentThread(g_vm);
}

/* ---------- JSON 辅助 ---------- */

static const char *jstr(const RJson *o, const char *key) {
    if (!o) return NULL;
    const RJson *v = rjson_obj_get(o, key);
    if (v && v->type == RJSON_STRING) return v->u.str.ptr;
    return NULL;
}/* ---------- 消息解析 ---------- */

/* 按魔数探测图片 MIME(默认 png; jpeg/gif/webp/heic 常见格式) */
static const char *sniff_mime(const uint8_t *d, size_t n) {
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8) return "image/jpeg";
    if (n >= 6 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F') return "image/gif";
    if (n >= 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') return "image/png";
    if (n >= 12 && d[0] == 'R' && d[1] == 'I' && d[2] == 'F' && d[3] == 'F') return "image/webp";
    if (n >= 12 && d[0] == 0x00 && d[1] == 0x00 && d[2] == 0x00 && d[3] == 0x18 &&
        memcmp(d + 4, "ftypheic", 8) == 0) return "image/heic";
    if (n >= 12 && d[0] == 0x00 && d[1] == 0x00 && d[2] == 0x00 && d[3] == 0x18 &&
        memcmp(d + 4, "ftypheix", 8) == 0) return "image/heic";
    return "image/png";
}

static int parse_history(Arena *a, const char *history_json,
                         const RikkaMessage **out, size_t cap, size_t *n_out) {
    size_t err = 0;
    RJson *v = rjson_parse(a, history_json, strlen(history_json), &err);
    if (!v || v->type != RJSON_ARRAY) return -1;
    size_t n = 0;
    for (size_t i = 0; i < v->u.arr.count && n < cap; i++) {
        const RJson *e = v->u.arr.items[i];
        if (e->type != RJSON_OBJECT) continue;
        const char *role = jstr(e, "role");
        const char *content = jstr(e, "content");
        if (!role || !content) continue;
        RikkaRole r = RIKKA_ROLE_USER;
        if (strcmp(role, "system") == 0) r = RIKKA_ROLE_SYSTEM;
        else if (strcmp(role, "assistant") == 0) r = RIKKA_ROLE_ASSISTANT;
        RikkaMessage *m = rmsg_new(a, r);
        RikkaPart *p = rmsg_add_part(a, m, RIKKA_PART_TEXT);
        p->data = content;
        p->len = strlen(content);
        /* 可选图片：image_path 字段 → 读文件 → base64 data URI → IMAGE part；
           或 image_url 字段（http/https）→ 直接作为 IMAGE part */
        const char *img = jstr(e, "image_path");
        const char *img_url = jstr(e, "image_url");
        if (img && img[0]) {
            FILE *f = fopen(img, "rb");
            if (f) {
                Buf raw;
                buf_init(&raw);
                char rb[8192];
                size_t rn;
                while ((rn = fread(rb, 1, sizeof(rb), f)) > 0) {
                    if (raw.len + rn > 16 * 1024 * 1024) break; /* 超大图截断 */
                    buf_append(&raw, rb, rn);
                }
                fclose(f);
                if (raw.len > 0) {
                    const char *mime = sniff_mime((const uint8_t *)raw.data, raw.len);
                    size_t mlen = strlen(mime);          /* "image/jpeg" 等 */
                    const char PREFIX[] = "data:;base64,";
                    size_t pfx = sizeof(PREFIX) - 1 + mlen; /* 前缀总长 */
                    size_t cap_b64 = ((raw.len + 2) / 3) * 4 + 64;
                    char *b64 = (char *)arena_alloc(a, 8, cap_b64 + pfx);
                    size_t b64len = b64_encode(raw.data, raw.len, b64 + pfx);
                    memcpy(b64, "data:", 5);
                    memcpy(b64 + 5, mime, mlen);
                    memcpy(b64 + 5 + mlen, ";base64,", 8);
                    b64[pfx + b64len] = '\0';
                    RikkaPart *ip = rmsg_add_part(a, m, RIKKA_PART_IMAGE);
                    ip->data = b64;
                    ip->len = pfx + b64len;
                }
                buf_free(&raw);
            }
        } else if (img_url && img_url[0]) {
            RikkaPart *ip = rmsg_add_part(a, m, RIKKA_PART_IMAGE);
            ip->data = img_url;
            ip->len = strlen(img_url);
        } else {
            /* data: URI(image_data)——已是 data:<mime>;base64,<payload> 格式直通 */
            const char *img_data = jstr(e, "image_data");
            if (img_data && strncmp(img_data, "data:", 5) == 0) {
                RikkaPart *ip = rmsg_add_part(a, m, RIKKA_PART_IMAGE);
                ip->data = img_data;
                ip->len = strlen(img_data);
            }
        }
        out[n++] = m;
    }
    *n_out = n;
    return n == 0 ? -1 : 0;
}

/* JSON 字符串转义(追加到 Buf): 引号/反斜杠/全部控制字符 */
static void jesc(Buf *out, const char *s) {
    for (const char *q = s; *q; q++) {
        unsigned char ch = (unsigned char)*q;
        switch (ch) {
            case '"':  buf_append_str(out, "\\\""); break;
            case '\\': buf_append_str(out, "\\\\"); break;
            case '\n': buf_append_str(out, "\\n"); break;
            case '\r': buf_append_str(out, "\\r"); break;
            case '\t': buf_append_str(out, "\\t"); break;
            case '\b': buf_append_str(out, "\\b"); break;
            case '\f': buf_append_str(out, "\\f"); break;
            default:
                if (ch < 0x20) {
                    char ub[8];
                    int uk = snprintf(ub, sizeof(ub), "\\u%04x", ch);
                    buf_append(out, ub, (size_t)uk);
                } else {
                    buf_append_byte(out, ch);
                }
        }
    }
}

/* ---------- 设备工具（JNI 反调 Kotlin DeviceTools） ---------- */

static jclass g_dev_cls = NULL;
static jmethodID g_dev_ask_user, g_dev_clip, g_dev_clip_read, g_dev_tts, g_dev_cal, g_dev_cal_create,
                 g_dev_st, g_dev_js, g_dev_web_search, g_dev_mem;
static jclass g_store_cls = NULL;
static jmethodID g_store_recent, g_store_search;

static JNIEnv *env_of(void) {
    JNIEnv *env = NULL;
    if (!g_vm) return NULL;
    if ((*g_vm)->GetEnv(g_vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK) return NULL;
    return env;
}

static char *jstrdup_utf(JNIEnv *env, jstring s) {
    if (!s) return NULL;
    const char *c = (*env)->GetStringUTFChars(env, s, NULL);
    if (!c) return NULL;
    char *r = strdup(c);
    (*env)->ReleaseStringUTFChars(env, s, c);
    return r;
}

static void ensure_device_cls(JNIEnv *env) {
    if (g_dev_cls) return;
    jclass c = (*env)->FindClass(env, "dev/rikkahub/ce/DeviceTools");
    if (!c) return;
    g_dev_cls = (jclass)(*env)->NewGlobalRef(env, c);
    g_dev_ask_user = (*env)->GetStaticMethodID(env, g_dev_cls, "askUser",
                                               "(Ljava/lang/String;)Ljava/lang/String;");
    g_dev_clip = (*env)->GetStaticMethodID(env, g_dev_cls, "clipboardWrite",
                                           "(Ljava/lang/String;)Z");
    g_dev_tts = (*env)->GetStaticMethodID(env, g_dev_cls, "ttsSpeak",
                                          "(Ljava/lang/String;)Z");
    g_dev_cal = (*env)->GetStaticMethodID(env, g_dev_cls, "calendarQuery",
                                          "(Ljava/lang/String;)Ljava/lang/String;");
    g_dev_st = (*env)->GetStaticMethodID(env, g_dev_cls, "screenTimeQuery",
                                         "(Ljava/lang/String;)Ljava/lang/String;");
    g_dev_js = (*env)->GetStaticMethodID(env, g_dev_cls, "javascriptEval",
                                         "(Ljava/lang/String;)Ljava/lang/String;");
    (*env)->DeleteLocalRef(env, c);
}

static char *jni_ask_user(const char *question, void *ud) {
    (void)ud;
    JNIEnv *env = env_of();
    if (!env) return NULL;
    ensure_device_cls(env);
    if (!g_dev_cls || !g_dev_ask_user) return NULL;
    jstring q = (*env)->NewStringUTF(env, question);
    jstring r = (jstring)(*env)->CallStaticObjectMethod(env, g_dev_cls, g_dev_ask_user, q);
    (*env)->DeleteLocalRef(env, q);
    return jstrdup_utf(env, r);
}

static int jni_clipboard_write(const char *text, void *ud) {
    (void)ud;
    JNIEnv *env = env_of();
    if (!env) return 0;
    ensure_device_cls(env);
    if (!g_dev_cls || !g_dev_clip) return 0;
    jstring t = (*env)->NewStringUTF(env, text);
    jboolean ok = (*env)->CallStaticBooleanMethod(env, g_dev_cls, g_dev_clip, t);
    (*env)->DeleteLocalRef(env, t);
    return ok ? 1 : 0;
}

static int jni_tts_speak(const char *text, void *ud) {
    (void)ud;
    JNIEnv *env = env_of();
    if (!env) return 0;
    ensure_device_cls(env);
    if (!g_dev_cls || !g_dev_tts) return 0;
    jstring t = (*env)->NewStringUTF(env, text);
    jboolean ok = (*env)->CallStaticBooleanMethod(env, g_dev_cls, g_dev_tts, t);
    (*env)->DeleteLocalRef(env, t);
    return ok ? 1 : 0;
}

static char *jni_calendar_query(const char *args, void *ud) {
    (void)ud;
    JNIEnv *env = env_of();
    if (!env) return NULL;
    ensure_device_cls(env);
    if (!g_dev_cls || !g_dev_cal) return NULL;
    jstring a = (*env)->NewStringUTF(env, args);
    jstring r = (jstring)(*env)->CallStaticObjectMethod(env, g_dev_cls, g_dev_cal, a);
    (*env)->DeleteLocalRef(env, a);
    return jstrdup_utf(env, r);
}

static char *jni_screen_time_query(const char *args, void *ud) {
    (void)ud;
    JNIEnv *env = env_of();
    if (!env) return NULL;
    ensure_device_cls(env);
    if (!g_dev_cls || !g_dev_st) return NULL;
    jstring a = (*env)->NewStringUTF(env, args);
    jstring r = (jstring)(*env)->CallStaticObjectMethod(env, g_dev_cls, g_dev_st, a);
    (*env)->DeleteLocalRef(env, a);
    return jstrdup_utf(env, r);
}

static char *jni_javascript_eval(const char *code, void *ud) {
    (void)ud;
    JNIEnv *env = env_of();
    if (!env) return NULL;
    ensure_device_cls(env);
    if (!g_dev_cls || !g_dev_js) return NULL;
    jstring c = (*env)->NewStringUTF(env, code);
    jstring r = (jstring)(*env)->CallStaticObjectMethod(env, g_dev_cls, g_dev_js, c);
    (*env)->DeleteLocalRef(env, c);
    return jstrdup_utf(env, r);
}

/* ---------- 会话存储（反调 Kotlin ChatStore） ---------- */

static void ensure_store_cls(JNIEnv *env) {
    if (g_store_cls) return;
    jclass c = (*env)->FindClass(env, "dev/rikkahub/ce/ChatStore");
    if (!c) return;
    g_store_cls = (jclass)(*env)->NewGlobalRef(env, c);
    g_store_recent = (*env)->GetStaticMethodID(env, g_store_cls, "recentChats",
                                               "(I)Ljava/lang/String;");
    g_store_search = (*env)->GetStaticMethodID(env, g_store_cls, "conversationSearch",
                                               "(Ljava/lang/String;)Ljava/lang/String;");
    (*env)->DeleteLocalRef(env, c);
}

static char *jni_clipboard_read(void *ud) {
    (void)ud;
    JNIEnv *env = env_of();
    if (!env) return NULL;
    if (!g_dev_cls) {
        jclass c = (*env)->FindClass(env, "dev/rikkahub/ce/DeviceTools");
        if (!c) return NULL;
        g_dev_cls = (*env)->NewGlobalRef(env, c);
    }
    if (!g_dev_clip_read) {
        g_dev_clip_read = (*env)->GetStaticMethodID(env, g_dev_cls, "clipboardRead",
                                                    "()Ljava/lang/String;");
        if (!g_dev_clip_read) return NULL;
    }
    jstring jr = (jstring)(*env)->CallStaticObjectMethod(env, g_dev_cls, g_dev_clip_read);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return NULL;
    }
    if (!jr) return NULL;
    const char *r = (*env)->GetStringUTFChars(env, jr, NULL);
    if (!r) return NULL;
    char *out = strdup(r);
    (*env)->ReleaseStringUTFChars(env, jr, r);
    (*env)->DeleteLocalRef(env, jr);
    return out;
}

static char *jni_calendar_create(const char *args, void *ud) {
    (void)ud;
    JNIEnv *env = env_of();
    if (!env || !args) return NULL;
    if (!g_dev_cls) {
        jclass c = (*env)->FindClass(env, "dev/rikkahub/ce/DeviceTools");
        if (!c) return NULL;
        g_dev_cls = (*env)->NewGlobalRef(env, c);
    }
    if (!g_dev_cal_create) {
        g_dev_cal_create = (*env)->GetStaticMethodID(env, g_dev_cls, "calendarCreate",
                                                     "(Ljava/lang/String;)Ljava/lang/String;");
        if (!g_dev_cal_create) return NULL;
    }
    jstring ja = (*env)->NewStringUTF(env, args);
    jstring jr = (jstring)(*env)->CallStaticObjectMethod(env, g_dev_cls, g_dev_cal_create, ja);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return NULL;
    }
    if (!jr) return NULL;
    const char *r = (*env)->GetStringUTFChars(env, jr, NULL);
    if (!r) return NULL;
    char *out = strdup(r);
    (*env)->ReleaseStringUTFChars(env, jr, r);
    (*env)->DeleteLocalRef(env, ja);
    (*env)->DeleteLocalRef(env, jr);
    return out;
}

static char *jni_web_search(const char *query, void *ud) {
    (void)ud;
    JNIEnv *env = env_of();
    if (!env || !query) return NULL;
    if (!g_dev_cls) {
        jclass c = (*env)->FindClass(env, "dev/rikkahub/ce/DeviceTools");
        if (!c) return NULL;
        g_dev_cls = (*env)->NewGlobalRef(env, c);
    }
    if (!g_dev_web_search) {
        g_dev_web_search = (*env)->GetStaticMethodID(env, g_dev_cls, "webSearch",
                                                     "(Ljava/lang/String;)Ljava/lang/String;");
        if (!g_dev_web_search) return NULL;
    }
    jstring jq = (*env)->NewStringUTF(env, query);
    jstring jr = (jstring)(*env)->CallStaticObjectMethod(env, g_dev_cls, g_dev_web_search, jq);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return NULL;
    }
    if (!jr) return NULL;
    const char *r = (*env)->GetStringUTFChars(env, jr, NULL);
    if (!r) return NULL;
    char *out = strdup(r);
    (*env)->ReleaseStringUTFChars(env, jr, r);
    (*env)->DeleteLocalRef(env, jq);
    (*env)->DeleteLocalRef(env, jr);
    return out;
}

/* memory_tool 反调: assistant_id 从 providerJson 传入, 存于 jni_ctx_mem_aid */
static char *jni_ctx_mem_aid = NULL;  /* strdup 的 assistant_id; 每轮 chat 重建 */

static char *jni_memory(const char *action, int64_t id, const char *content, void *ud) {
    (void)ud;
    JNIEnv *env = env_of();
    if (!env || !action || !jni_ctx_mem_aid) return NULL;
    if (!g_dev_cls) {
        jclass c = (*env)->FindClass(env, "dev/rikkahub/ce/DeviceTools");
        if (!c) return NULL;
        g_dev_cls = (*env)->NewGlobalRef(env, c);
    }
    if (!g_dev_mem) {
        g_dev_mem = (*env)->GetStaticMethodID(
            env, g_dev_cls, "memoryAction",
            "(Ljava/lang/String;JLjava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
        if (!g_dev_mem) return NULL;
    }
    jstring ja = (*env)->NewStringUTF(env, action);
    jstring jc = (*env)->NewStringUTF(env, content ? content : "");
    jstring jid = (*env)->NewStringUTF(env, jni_ctx_mem_aid);
    jstring jr = (jstring)(*env)->CallStaticObjectMethod(
        env, g_dev_cls, g_dev_mem, ja, (jlong)id, jc, jid);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return NULL;
    }
    if (!jr) return NULL;
    const char *r = (*env)->GetStringUTFChars(env, jr, NULL);
    if (!r) return NULL;
    char *out = strdup(r);
    (*env)->ReleaseStringUTFChars(env, jr, r);
    (*env)->DeleteLocalRef(env, ja);
    (*env)->DeleteLocalRef(env, jc);
    (*env)->DeleteLocalRef(env, jid);
    (*env)->DeleteLocalRef(env, jr);
    return out;
}

static char *jni_memory_create(const char *content, void *ud) {
    return jni_memory("create", 0, content, ud);
}

static char *jni_memory_edit(int64_t id, const char *content, void *ud) {
    return jni_memory("edit", id, content, ud);
}

static int jni_memory_delete(int64_t id, void *ud) {
    char *r = jni_memory("delete", id, NULL, ud);
    if (!r) return -1;
    free(r);
    return 0;
}

static char *jni_recent_chats(int limit, void *ud) {
    (void)ud;
    JNIEnv *env = env_of();
    if (!env) return NULL;
    ensure_store_cls(env);
    if (!g_store_cls || !g_store_recent) return NULL;
    jstring r = (jstring)(*env)->CallStaticObjectMethod(env, g_store_cls,
                                                        g_store_recent, (jint)limit);
    return jstrdup_utf(env, r);
}

static char *jni_conversation_search(const char *query, void *ud) {
    (void)ud;
    JNIEnv *env = env_of();
    if (!env) return NULL;
    ensure_store_cls(env);
    if (!g_store_cls || !g_store_search) return NULL;
    jstring q = (*env)->NewStringUTF(env, query);
    jstring r = (jstring)(*env)->CallStaticObjectMethod(env, g_store_cls,
                                                        g_store_search, q);
    (*env)->DeleteLocalRef(env, q);
    return jstrdup_utf(env, r);
}

/* ---------- OCR（rk_ocr_image 暴露） ---------- */

static const char B64_TBL[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t b64_encode(const uint8_t *in, size_t in_len, char *out) {
    size_t o = 0;
    for (size_t i = 0; i + 2 < in_len || i < in_len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < in_len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < in_len) v |= in[i + 2];
        out[o++] = B64_TBL[(v >> 18) & 63];
        out[o++] = B64_TBL[(v >> 12) & 63];
        out[o++] = i + 1 < in_len ? B64_TBL[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < in_len ? B64_TBL[v & 63] : '=';
    }
    return o;
}

JNIEXPORT jstring JNICALL
Java_dev_rikkahub_ce_Engine_nativeOcr(JNIEnv *env, jclass cls,
                                            jstring provider_json,
                                            jstring image_path) {
    (void)cls;
    const char *pj = provider_json ? (*env)->GetStringUTFChars(env, provider_json, NULL) : NULL;
    const char *ip = image_path ? (*env)->GetStringUTFChars(env, image_path, NULL) : NULL;
    if (!pj || !ip) {
        if (pj) (*env)->ReleaseStringUTFChars(env, provider_json, pj);
        if (ip) (*env)->ReleaseStringUTFChars(env, image_path, ip);
        return (*env)->NewStringUTF(env, "{\"ok\":false,\"error\":\"bad args\"}");
    }
    /* 读图片 → base64 data URI */
    FILE *f = fopen(ip, "rb");
    if (!f) {
        (*env)->ReleaseStringUTFChars(env, provider_json, pj);
        (*env)->ReleaseStringUTFChars(env, image_path, ip);
        return (*env)->NewStringUTF(env, "{\"ok\":false,\"error\":\"image not readable\"}");
    }
    Buf raw;
    buf_init(&raw);
    char rb[8192];
    size_t n;
    while ((n = fread(rb, 1, sizeof(rb), f)) > 0) {
        if (raw.len + n > 16 * 1024 * 1024) { /* 超大图保护(16MB) */
            buf_free(&raw);
            fclose(f);
            return (*env)->NewStringUTF(env,
                "{\"ok\":false,\"error\":\"image too large\"}");
        }
        buf_append(&raw, rb, n);
    }
    fclose(f);
    char *b64 = (char *)malloc(((raw.len + 2) / 3) * 4 + 1);
    if (!b64) { buf_free(&raw); return (*env)->NewStringUTF(env, "{\"ok\":false,\"error\":\"oom\"}"); }
    size_t b64len = b64_encode(raw.data, raw.len, b64);
    b64[b64len] = '\0';
    buf_free(&raw);

    /* 构造 data URI */
    char *data_uri = (char *)malloc(b64len + 64);
    if (!data_uri) { free(b64); return (*env)->NewStringUTF(env, "{\"ok\":false,\"error\":\"oom\"}"); }
    snprintf(data_uri, b64len + 64, "data:image/png;base64,%s", b64);
    free(b64);

    /* provider 配置 */
    Arena *a = arena_create(0);
    size_t jerr = 0;
    RJson *pv = rjson_parse(a, pj, strlen(pj), &jerr);
    RikkaProviderCfg cfg = {RIKKA_PROVIDER_OPENAI,
                            jstr(pv, "base_url") ? jstr(pv, "base_url") : "",
                            jstr(pv, "api_key") ? jstr(pv, "api_key") : "",
                            jstr(pv, "model") ? jstr(pv, "model") : "",
                            4096, 0, NULL, {0}, NULL, 0, -1, -1, NULL};
    char *text = NULL;
    char *detail = NULL;
    int rc = rk_ocr_image(&cfg, RK_PROMPT_OCR, data_uri, 120000, &text, &detail);

    Buf out;
    buf_init(&out);
    if (rc == 0 && text) {
        buf_append_str(&out, "{\"ok\":true,\"text\":");
        buf_append_byte(&out, '"');
                jesc(&out, text);
        buf_append_byte(&out, '"');
        buf_append_str(&out, "}");
    } else {
        const char *msg = (detail && detail[0]) ? detail : "ocr failed";
        buf_append_str(&out, "{\"ok\":false,\"error\":");
        buf_append_byte(&out, '"');
                jesc(&out, msg);
        buf_append_byte(&out, '"');
        buf_append_str(&out, "}");
    }
    jstring result = (*env)->NewStringUTF(env, (const char *)out.data);
    free(detail);
    free(text);
    free(data_uri);
    buf_free(&out);
    arena_destroy(a);
    (*env)->ReleaseStringUTFChars(env, provider_json, pj);
    (*env)->ReleaseStringUTFChars(env, image_path, ip);
    return result;
}

JNIEXPORT jstring JNICALL
Java_dev_rikkahub_ce_Engine_nativeGenerateTitle(JNIEnv *env, jclass cls,
                                                jstring provider_json,
                                                jstring content) {
    (void)cls;
    const char *pj = provider_json ? (*env)->GetStringUTFChars(env, provider_json, NULL) : NULL;
    const char *ct = content ? (*env)->GetStringUTFChars(env, content, NULL) : NULL;
    if (!pj || !ct) {
        if (pj) (*env)->ReleaseStringUTFChars(env, provider_json, pj);
        if (ct) (*env)->ReleaseStringUTFChars(env, content, ct);
        return (*env)->NewStringUTF(env, "{\"ok\":false,\"error\":\"bad args\"}");
    }
    Arena *a = arena_create(0);
    size_t jerr = 0;
    RJson *pv = rjson_parse(a, pj, strlen(pj), &jerr);
    RikkaProviderCfg cfg = {RIKKA_PROVIDER_OPENAI,
                            jstr(pv, "base_url") ? jstr(pv, "base_url") : "",
                            jstr(pv, "api_key") ? jstr(pv, "api_key") : "",
                            jstr(pv, "model") ? jstr(pv, "model") : "",
                            4096, 0, NULL, {0}, NULL, 0, -1, -1, NULL};

    /* system = 标题 prompt；user = 会话内容 */
    const char *names[2] = {"locale", "content"};
    const char *values[2] = {"zh-CN", ct};
    char *sys = rk_prompt_fill(a, RK_PROMPT_TITLE, names, values, 2);
    RikkaMessage *sm = rmsg_new(a, RIKKA_ROLE_SYSTEM);
    RikkaPart *sp = rmsg_add_part(a, sm, RIKKA_PART_TEXT);
    sp->data = sys;
    sp->len = strlen(sys);
    RikkaMessage *um = rmsg_new(a, RIKKA_ROLE_USER);
    RikkaPart *up = rmsg_add_part(a, um, RIKKA_PART_TEXT);
    up->data = ct;
    up->len = strlen(ct);
    const RikkaMessage *msgs[2] = {sm, um};
    RikkaStream out;
    rstream_init(&out, a, RIKKA_ROLE_ASSISTANT);
    char *detail = NULL;
    int rc = rp_chat_stream_cb(&cfg, msgs, 2, &out, 60000, NULL, NULL, NULL, NULL, &detail);
    char *text = NULL;
    if (rc == 0) {
        /* 提取文本 parts 拼接 */
        Buf b;
        buf_init(&b);
        for (size_t i = 0; i < out.msg->part_count; i++) {
            const RikkaPart *p = &out.msg->parts[i];
            if (p->type == RIKKA_PART_TEXT && p->data) buf_append(&b, p->data, p->len);
        }
        if (b.len > 0) {
            text = (char *)malloc(b.len + 1);
            if (text) {
                memcpy(text, b.data, b.len);
                text[b.len] = '\0';
            }
        }
        buf_free(&b);
    }
    rstream_destroy(&out);

    Buf out_json;
    buf_init(&out_json);
    if (text && text[0]) {
        buf_append_str(&out_json, "{\"ok\":true,\"title\":");
        buf_append_byte(&out_json, '"');
        jesc(&out_json, text);
        buf_append_byte(&out_json, '"');
        buf_append_str(&out_json, "}");
    } else {
        const char *msg = (detail && detail[0]) ? detail : "title generation failed";
        buf_append_str(&out_json, "{\"ok\":false,\"error\":");
        buf_append_byte(&out_json, '"');
                jesc(&out_json, msg);
        buf_append_byte(&out_json, '"');
        buf_append_str(&out_json, "}");
    }
    jstring result = (*env)->NewStringUTF(env, (const char *)out_json.data);
    free(detail);
    free(text);
    buf_free(&out_json);
    arena_destroy(a);
    (*env)->ReleaseStringUTFChars(env, provider_json, pj);
    (*env)->ReleaseStringUTFChars(env, content, ct);
    return result;
}

/* ---------- JNI 入口 ---------- */

JNIEXPORT jstring JNICALL
Java_dev_rikkahub_ce_Engine_nativeChat(JNIEnv *env, jclass cls,
                                             jstring provider_json,
                                             jstring history_json,
                                             jstring workspace_root,
                                             jobject callback) {
    (void)cls;
    const char *pj = provider_json ? (*env)->GetStringUTFChars(env, provider_json, NULL) : NULL;
    const char *hj = history_json ? (*env)->GetStringUTFChars(env, history_json, NULL) : NULL;
    const char *wr = workspace_root ? (*env)->GetStringUTFChars(env, workspace_root, NULL) : NULL;
    if (!pj || !hj) {
        if (pj) (*env)->ReleaseStringUTFChars(env, provider_json, pj);
        if (hj) (*env)->ReleaseStringUTFChars(env, history_json, hj);
        if (wr) (*env)->ReleaseStringUTFChars(env, workspace_root, wr);
        return (*env)->NewStringUTF(env, "{\"ok\":false,\"error\":\"bad args\"}");
    }

    Arena *a = arena_create(0);
    size_t jerr = 0;
    RJson *pv = rjson_parse(a, pj, strlen(pj), &jerr);
    const char *base_url = jstr(pv, "base_url");
    const char *api_key = jstr(pv, "api_key");
    const char *model = jstr(pv, "model");
    const char *err = NULL;

    /* 字段顺序: id, base_url, api_key, model, max_tokens, enable_cache_control, tools_json, retry */
    RikkaProviderCfg pcfg = {RIKKA_PROVIDER_OPENAI,
                             base_url ? base_url : "",
                             api_key ? api_key : "",
                             model ? model : "",
                             4096, 0, NULL, {0}, NULL, 0, -1, -1, NULL};
    /* 思考模式(DeepSeek 等): reasoning_effort / thinking */
    pcfg.reasoning_effort = jstr(pv, "reasoning_effort");
    if (jstr(pv, "thinking")) pcfg.thinking_enabled = 1;
    /* 采样参数与附加 body(QWEN_MT 翻译等; 缺省不写) */
    pcfg.temperature = -1;
    pcfg.top_p = -1;
    const RJson *j_temp = rjson_obj_get(pv, "temperature");
    if (j_temp && rjson_is(j_temp, RJSON_NUMBER)) pcfg.temperature = (float)j_temp->u.number;
    const RJson *j_tp = rjson_obj_get(pv, "top_p");
    if (j_tp && rjson_is(j_tp, RJSON_NUMBER)) pcfg.top_p = (float)j_tp->u.number;
    pcfg.custom_body = jstr(pv, "custom_body");

    /* memory_tool 反调目标 assistant_id(enableMemory 时 Kotlin 传入; 每轮重建) */
    free(jni_ctx_mem_aid);
    jni_ctx_mem_aid = NULL;
    const char *aid = jstr(pv, "assistant_id");
    if (aid && aid[0]) jni_ctx_mem_aid = strdup(aid);

    /* skills 根目录(Android: filesDir/skills; NULL 用默认 /skills) */
    const char *skills_root = jstr(pv, "skills_root");

    const RikkaMessage *msgs[64];
    size_t n_msgs = 0;
    if (parse_history(a, hj, msgs, 64, &n_msgs) != 0) {
        err = "history must be a non-empty JSON array";
    }

    /* 工具（内置 + 设备工具 + 会话工具 + workspace 沙箱） */
    RkToolRegistry reg;
    RkToolEnv tenv = {0};
    tenv.workspace_root = wr;   /* workspace 沙箱根（JNI 传入） */
    tenv.workspace_cwd = wr;    /* shell 默认工作目录 */
    tenv.ask_user = jni_ask_user;
    tenv.clipboard_write = jni_clipboard_write;
    tenv.tts_speak = jni_tts_speak;
    tenv.calendar_query = jni_calendar_query;
    tenv.screen_time_query = jni_screen_time_query;
    tenv.javascript_eval = jni_javascript_eval;
    /* 会话/搜索开关(对齐 turbo: enableWebSearch / enableRecentChatsReference) */
    const RJson *e_ws = rjson_obj_get(pv, "enable_web_search");
    const RJson *e_rc = rjson_obj_get(pv, "enable_recent_chats");
    int enable_web_search = !e_ws || (rjson_is(e_ws, RJSON_BOOL) && e_ws->u.boolean);
    int enable_recent = !e_rc || (rjson_is(e_rc, RJSON_BOOL) && e_rc->u.boolean);
    tenv.recent_chats = enable_recent ? jni_recent_chats : NULL;
    tenv.conversation_search = enable_recent ? jni_conversation_search : NULL;
    tenv.web_search = enable_web_search ? jni_web_search : NULL;
    tenv.memory_create = jni_memory_create;
    tenv.memory_edit = jni_memory_edit;
    tenv.memory_delete = jni_memory_delete;
    tenv.calendar_create = jni_calendar_create;
    tenv.clipboard_read = jni_clipboard_read;
    tenv.skills_root = skills_root;
    tenv.tool_whitelist = jstr(pv, "tool_whitelist");
    rk_tools_init(&reg);
    rk_tools_register_builtin(&reg, &tenv);

    /* 回调引用与方法 */
    JniCb jc;
    jc.env = env;
    jobject gcb = (*env)->NewGlobalRef(env, callback);
    jc.cb = gcb;
    jclass cbcls = (*env)->GetObjectClass(env, callback);
    if (!g_m_delta) {
        g_m_delta = (*env)->GetMethodID(env, cbcls, "onDelta", "(ILjava/lang/String;)V");
        g_m_tool_call = (*env)->GetMethodID(env, cbcls, "onToolCall", "(Ljava/lang/String;Ljava/lang/String;)V");
        g_m_tool_result = (*env)->GetMethodID(env, cbcls, "onToolResult", "(Ljava/lang/String;Ljava/lang/String;)V");
        g_m_finish = (*env)->GetMethodID(env, cbcls, "onFinish", "(ZLjava/lang/String;)V");
    }
    (*env)->DeleteLocalRef(env, cbcls);

    RkChatCallbacks cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.ud = &jc;
    cbs.on_delta = jni_delta;
    cbs.on_tool_call = jni_tool_call;
    cbs.on_tool_result = jni_tool_result;

    RkChatConfig cc;
    memset(&cc, 0, sizeof(cc));
    cc.provider = pcfg;
    cc.tools = &reg;
    cc.tool_env = &tenv;
    cc.timeout_ms = 120000;
    cc.cancel_flag = &g_cancel;

    char *final_text = NULL;
    char *chat_err = NULL;
    RikkaSessionStats cstats;
    memset(&cstats, 0, sizeof(cstats));
    g_cancel = 0;
    int rc = err ? -1 : rk_chat_run(&cc, &cbs, msgs, n_msgs, &final_text, &chat_err, &cstats);

    /* 完成回调 */
    jstring ferr = (rc != 0 && chat_err) ? (*env)->NewStringUTF(env, chat_err) : NULL;
    (*env)->CallVoidMethod(env, gcb, g_m_finish, rc == 0, ferr);
    if (ferr) (*env)->DeleteLocalRef(env, ferr);

    /* 结果 JSON */
    Buf out;
    buf_init(&out);
    if (rc == 0 && final_text) {
        buf_append_str(&out, "{\"ok\":true,\"text\":");
        buf_append_byte(&out, '"');
                jesc(&out, final_text);
        buf_append_byte(&out, '"');
        if (cstats.prompt_tokens > 0 || cstats.completion_tokens > 0) {
            char ub[192];
            int uk = snprintf(ub, sizeof(ub),
                              ",\"usage\":{\"prompt_tokens\":%d,\"completion_tokens\":%d,\"cached_tokens\":%d}",
                              cstats.prompt_tokens, cstats.completion_tokens,
                              cstats.cached_tokens);
            buf_append(&out, ub, (size_t)uk);
        }
        if (cstats.http_status > 0) {
            char rb[96];
            int rk2 = snprintf(rb, sizeof(rb),
                               ",\"request\":{\"status\":%d,\"duration_ms\":%ld}",
                               cstats.http_status, cstats.duration_ms);
            buf_append(&out, rb, (size_t)rk2);
        }
        buf_append_str(&out, "}");
    } else {
        buf_append_str(&out, "{\"ok\":false,\"error\":");
        jstring es = (*env)->NewStringUTF(env, chat_err ? chat_err : (err ? err : "chat failed"));
        const char *es_c = (*env)->GetStringUTFChars(env, es, NULL);
        buf_append_byte(&out, '"');
                jesc(&out, es_c);
        buf_append_byte(&out, '"');
        buf_append_str(&out, "}");
        (*env)->ReleaseStringUTFChars(env, es, es_c);
        (*env)->DeleteLocalRef(env, es);
    }
    jstring result = (*env)->NewStringUTF(env, (const char *)out.data);

    /* 清理 */
    (*env)->DeleteGlobalRef(env, gcb);
    free(final_text);
    free(chat_err);
    rk_tools_destroy(&reg);
    buf_free(&out);
    arena_destroy(a);
    (*env)->ReleaseStringUTFChars(env, provider_json, pj);
    (*env)->ReleaseStringUTFChars(env, history_json, hj);
    if (wr) (*env)->ReleaseStringUTFChars(env, workspace_root, wr);
    return result;
}

JNIEXPORT void JNICALL
Java_dev_rikkahub_ce_Engine_nativeSetCancel(JNIEnv *env, jclass cls,
                                                  jboolean cancel) {
    (void)env;
    (void)cls;
    g_cancel = cancel ? 1 : 0;
}
