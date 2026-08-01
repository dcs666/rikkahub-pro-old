/*
 * RikkaHub CE: 引擎 JNI 桥。
 * 暴露: nativeChat(providerJson, historyJson, callback) / nativeSetCancel(v)
 * 线程模型: nativeChat 阻塞调用方线程（Kotlin 侧放协程 IO 线程）;
 * 回调在调用线程同步触发（增量/工具/完成）。
 */
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rikka/ai/chat.h"
#include "rikka/ai/tool.h"
#include "rikka/core/message.h"
#include "rikka/json/json.h"
#include "rikka/util/arena.h"

static volatile int g_cancel = 0;

/* ---------- 回调桥 ---------- */

typedef struct {
    JNIEnv *env;
    jobject cb; /* 全局引用 */
} JniCb;

static jmethodID g_m_delta, g_m_tool_call, g_m_tool_result, g_m_finish;

static void jni_delta(void *ud, int kind, const char *data, size_t len) {
    JniCb *jc = (JniCb *)ud;
    /* NewStringUTF 需要 NUL 结尾：临时拷贝（增量块通常 <4KB） */
    char tmp[4096];
    const char *s = data;
    if (len >= sizeof(tmp)) len = sizeof(tmp) - 1;
    memcpy(tmp, data, len);
    tmp[len] = '\0';
    s = tmp;
    jstring js = (*jc->env)->NewStringUTF(jc->env, s);
    if (js) {
        (*jc->env)->CallVoidMethod(jc->env, jc->cb, g_m_delta, (jint)kind, js);
        (*jc->env)->DeleteLocalRef(jc->env, js);
    }
}

static void jni_tool_call(void *ud, const char *name, const char *args) {
    JniCb *jc = (JniCb *)ud;
    jstring n = (*jc->env)->NewStringUTF(jc->env, name);
    jstring a = (*jc->env)->NewStringUTF(jc->env, args);
    if (n && a) (*jc->env)->CallVoidMethod(jc->env, jc->cb, g_m_tool_call, n, a);
    if (n) (*jc->env)->DeleteLocalRef(jc->env, n);
    if (a) (*jc->env)->DeleteLocalRef(jc->env, a);
}

static void jni_tool_result(void *ud, const char *name, const char *result) {
    JniCb *jc = (JniCb *)ud;
    jstring n = (*jc->env)->NewStringUTF(jc->env, name);
    jstring r = (*jc->env)->NewStringUTF(jc->env, result);
    if (n && r) (*jc->env)->CallVoidMethod(jc->env, jc->cb, g_m_tool_result, n, r);
    if (n) (*jc->env)->DeleteLocalRef(jc->env, n);
    if (r) (*jc->env)->DeleteLocalRef(jc->env, r);
}

/* ---------- JSON 辅助 ---------- */

static const char *jstr(const RJson *o, const char *key) {
    if (!o) return NULL;
    const RJson *v = rjson_obj_get(o, key);
    if (v && v->type == RJSON_STRING) return v->u.str.ptr;
    return NULL;
}/* ---------- 消息解析 ---------- */

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
        out[n++] = m;
    }
    *n_out = n;
    return n == 0 ? -1 : 0;
}

/* ---------- JNI 入口 ---------- */

JNIEXPORT jstring JNICALL
Java_me_rerere_rikkahub_ce_Engine_nativeChat(JNIEnv *env, jclass cls,
                                             jstring provider_json,
                                             jstring history_json,
                                             jobject callback) {
    (void)cls;
    const char *pj = provider_json ? (*env)->GetStringUTFChars(env, provider_json, NULL) : NULL;
    const char *hj = history_json ? (*env)->GetStringUTFChars(env, history_json, NULL) : NULL;
    if (!pj || !hj) {
        if (pj) (*env)->ReleaseStringUTFChars(env, provider_json, pj);
        if (hj) (*env)->ReleaseStringUTFChars(env, history_json, hj);
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
                             4096, 0, NULL, {0}};

    const RikkaMessage *msgs[64];
    size_t n_msgs = 0;
    if (parse_history(a, hj, msgs, 64, &n_msgs) != 0) {
        err = "history must be a non-empty JSON array";
    }

    /* 工具（内置最小集；设备工具由壳层在 B3 接入） */
    RkToolRegistry reg;
    RkToolEnv tenv = {0};
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
    g_cancel = 0;
    int rc = err ? -1 : rk_chat_run(&cc, &cbs, msgs, n_msgs, &final_text, &chat_err);

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
        for (const char *q = final_text; *q; q++) {
            if (*q == '"' || *q == '\\') {
                buf_append_byte(&out, '\\');
                buf_append_byte(&out, (uint8_t)*q);
            } else if (*q == '\n') {
                buf_append_str(&out, "\\n");
            } else if (*q == '\r') {
                buf_append_str(&out, "\\r");
            } else if (*q == '\t') {
                buf_append_str(&out, "\\t");
            } else {
                buf_append_byte(&out, (uint8_t)*q);
            }
        }
        buf_append_byte(&out, '"');
        buf_append_str(&out, "}");
    } else {
        buf_append_str(&out, "{\"ok\":false,\"error\":");
        jstring es = (*env)->NewStringUTF(env, chat_err ? chat_err : (err ? err : "chat failed"));
        const char *es_c = (*env)->GetStringUTFChars(env, es, NULL);
        buf_append_byte(&out, '"');
        for (const char *q = es_c; *q; q++) {
            if (*q == '"' || *q == '\\') {
                buf_append_byte(&out, '\\');
                buf_append_byte(&out, (uint8_t)*q);
            } else if (*q == '\n') {
                buf_append_str(&out, "\\n");
            } else {
                buf_append_byte(&out, (uint8_t)*q);
            }
        }
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
    return result;
}

JNIEXPORT void JNICALL
Java_me_rerere_rikkahub_ce_Engine_nativeSetCancel(JNIEnv *env, jclass cls,
                                                  jboolean cancel) {
    (void)env;
    (void)cls;
    g_cancel = cancel ? 1 : 0;
}
