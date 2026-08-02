/*
 * 图像 OCR（见 ocr.h）。复用 provider 流式管线。
 */
#define _POSIX_C_SOURCE 200809L
#include "rikka/ai/ocr.h"
#include "rikka/core/buffer.h"
#include "rikka/core/message.h"
#include "rikka/util/arena.h"
#include <stdlib.h>
#include <string.h>

int rk_ocr_image(const RikkaProviderCfg *cfg, const char *ocr_prompt,
                 const char *image_path, int timeout_ms, char **text_out,
                 char **err_out) {
    if (!cfg || !ocr_prompt || !image_path || !text_out) return -1;
    Arena *a = arena_create(0);
    RikkaMessage *sys = rmsg_new(a, RIKKA_ROLE_SYSTEM);
    RikkaPart *sp = rmsg_add_part(a, sys, RIKKA_PART_TEXT);
    sp->data = ocr_prompt;
    sp->len = strlen(ocr_prompt);
    RikkaMessage *usr = rmsg_new(a, RIKKA_ROLE_USER);
    RikkaPart *up = rmsg_add_part(a, usr, RIKKA_PART_IMAGE);
    up->data = image_path;
    up->len = strlen(image_path);
    const RikkaMessage *msgs[2] = {sys, usr};
    RikkaStream out;
    rstream_init(&out, a, RIKKA_ROLE_ASSISTANT);
    char *detail = NULL;
    int rc = rp_chat_stream_cb(cfg, msgs, 2, &out, timeout_ms, NULL, NULL, NULL, NULL, &detail);
    /* 收集文本 parts */
    char *text = NULL;
    if (rc == 0 && out.msg) {
        Buf b;
        buf_init(&b);
        for (size_t i = 0; i < out.msg->part_count; i++) {
            const RikkaPart *p = &out.msg->parts[i];
            if (p->type == RIKKA_PART_TEXT && p->data) {
                buf_append(&b, p->data, p->len);
            }
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
    arena_destroy(a);
    if (!text) {
        if (err_out) *err_out = detail;
        else free(detail);
        return -1;
    }
    free(detail);
    *text_out = text;
    return 0;
}
