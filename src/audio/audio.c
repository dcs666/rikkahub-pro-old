#define _POSIX_C_SOURCE 200809L
#include "rikka/audio/audio.h"
#include "rikka/http/http.h"
#include "rikka/json/json.h"
#include "rikka/util/arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rk_audio_free(RkAudio *a) {
    if (a) {
        free(a->data);
        a->data = NULL;
        a->len = 0;
    }
}

int rk_tts_openai(const char *api_key, const char *text, const char *voice, RkAudio *out) {
    if (!api_key || !text || !out) return -1;
    /* 构建请求 JSON */
    char body[8192];
    int n = snprintf(body, sizeof(body),
                     "{\"model\":\"tts-1\",\"input\":\"%s\",\"voice\":\"%s\"}",
                     text, voice ? voice : "alloy");
    if (n < 0 || (size_t)n >= sizeof(body)) return -1;
    /* HTTP POST */
    RHttpConn *c = rhttp_connect("api.openai.com", 443, 1, 30000);
    if (!c) return -1;
    const char *headers[] = {
        "Authorization", NULL, /* 动态 */
        "Content-Type", "application/json",
        NULL
    };
    char auth[512];
    snprintf(auth, sizeof(auth), "Bearer %s", api_key);
    headers[1] = auth;
    if (rhttp_send(c, "POST", "/v1/audio/speech", headers, body, (size_t)n) != 0) {
        rhttp_close(c);
        return -1;
    }
    RHttpResp resp;
    if (rhttp_read_headers(c, &resp, 30000) != 0) {
        rhttp_close(c);
        return -1;
    }
    if (resp.status != 200) {
        rhttp_close(c);
        return -1;
    }
    /* 读响应体（MP3） */
    uint8_t *data = NULL;
    size_t len = 0, cap = 0;
    uint8_t buf[16384];
    for (;;) {
        ssize_t r = rhttp_read_body(c, (char *)buf, sizeof(buf), 30000);
        if (r < 0) break;
        if (r == 0) break;
        if (len + (size_t)r > cap) {
            size_t nc = cap ? cap * 2 : 65536;
            uint8_t *nd = (uint8_t *)realloc(data, nc);
            if (!nd) break;
            data = nd;
            cap = nc;
        }
        memcpy(data + len, buf, (size_t)r);
        len += (size_t)r;
    }
    rhttp_close(c);
    if (!data || len == 0) {
        free(data);
        return -1;
    }
    out->data = data;
    out->len = len;
    strcpy(out->format, "mp3");
    return 0;
}

int rk_asr_openai(const char *api_key, const uint8_t *audio, size_t len,
                  const char *format, char **text) {
    if (!api_key || !audio || !text) return -1;
    /* multipart/form-data 构建 */
    char boundary[64] = "----RikkaBoundary123456";
    char body[65536];
    size_t off = 0;
    /* file part */
    off += snprintf(body + off, sizeof(body) - off,
                    "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.%s\"\r\n"
                    "Content-Type: audio/%s\r\n\r\n",
                    boundary, format ? format : "mp3", format ? format : "mp3");
    if (off + len + 100 > sizeof(body)) return -1;
    memcpy(body + off, audio, len);
    off += len;
    off += snprintf(body + off, sizeof(body) - off,
                    "\r\n--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-1\r\n",
                    boundary);
    off += snprintf(body + off, sizeof(body) - off, "\r\n--%s--\r\n", boundary);
    /* HTTP POST */
    RHttpConn *c = rhttp_connect("api.openai.com", 443, 1, 60000);
    if (!c) return -1;
    char auth[512];
    snprintf(auth, sizeof(auth), "Bearer %s", api_key);
    char content_type[128];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
    const char *headers[] = {
        "Authorization", auth,
        "Content-Type", content_type,
        NULL
    };
    if (rhttp_send(c, "POST", "/v1/audio/transcriptions", headers, body, off) != 0) {
        rhttp_close(c);
        return -1;
    }
    RHttpResp resp;
    if (rhttp_read_headers(c, &resp, 60000) != 0) {
        rhttp_close(c);
        return -1;
    }
    if (resp.status != 200) {
        rhttp_close(c);
        return -1;
    }
    /* 读响应 JSON */
    char resp_body[65536];
    size_t resp_len = 0;
    for (;;) {
        ssize_t r = rhttp_read_body(c, resp_body + resp_len, sizeof(resp_body) - resp_len - 1, 60000);
        if (r <= 0) break;
        resp_len += (size_t)r;
    }
    resp_body[resp_len] = '\0';
    rhttp_close(c);
    /* 解析 JSON {"text": "..."} */
    Arena *a = arena_create(0);
    size_t err = 0;
    RJson *v = rjson_parse(a, resp_body, resp_len, &err);
    if (!v) { arena_destroy(a); return -1; }
    const RJson *t = rjson_obj_get(v, "text");
    if (!t || t->type != RJSON_STRING) { arena_destroy(a); return -1; }
    *text = strndup(t->u.str.ptr, t->u.str.len);
    arena_destroy(a);
    return *text ? 0 : -1;
}

int rk_wav_encode(const uint8_t *pcm, size_t len, int sample_rate, int channels,
                  uint8_t **out, size_t *out_len) {
    if (!pcm || !out) return -1;
    /* WAV header (44 bytes) + PCM data */
    size_t total = 44 + len;
    uint8_t *wav = (uint8_t *)malloc(total);
    if (!wav) return -1;
    /* RIFF header */
    memcpy(wav, "RIFF", 4);
    uint32_t chunk_size = 36 + len;
    memcpy(wav + 4, &chunk_size, 4);
    memcpy(wav + 8, "WAVE", 4);
    /* fmt chunk */
    memcpy(wav + 12, "fmt ", 4);
    uint32_t fmt_size = 16;
    memcpy(wav + 16, &fmt_size, 4);
    uint16_t audio_format = 1; /* PCM */
    memcpy(wav + 20, &audio_format, 2);
    uint16_t num_channels = (uint16_t)channels;
    memcpy(wav + 22, &num_channels, 2);
    uint32_t sr = (uint32_t)sample_rate;
    memcpy(wav + 24, &sr, 4);
    uint32_t byte_rate = sr * channels * 2; /* 16-bit */
    memcpy(wav + 28, &byte_rate, 4);
    uint16_t block_align = channels * 2;
    memcpy(wav + 32, &block_align, 2);
    uint16_t bits_per_sample = 16;
    memcpy(wav + 34, &bits_per_sample, 2);
    /* data chunk */
    memcpy(wav + 36, "data", 4);
    uint32_t data_size = len;
    memcpy(wav + 40, &data_size, 4);
    memcpy(wav + 44, pcm, len);
    *out = wav;
    *out_len = total;
    return 0;
}
