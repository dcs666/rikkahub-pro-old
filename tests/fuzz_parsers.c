/*
 * 解析器 fuzz：确定性伪随机畸形输入喂 rjson_stream / rbin_parse / md_parse_all。
 * 配合 UBSan 运行（CI 中 make build/fuzz_parsers 后执行）——确保不崩、无 UB。
 * 用法: ./build/fuzz_parsers [rounds]
 */
#define _POSIX_C_SOURCE 200809L
#include "rikka/json/json.h"
#include "rikka/data/rbin.h"
#include "rikka/markdown/md.h"
#include "rikka/util/arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t g_rng = 0x9E3779B97F4A7C15ULL;
static uint32_t xorshift(void) {
    g_rng ^= g_rng << 7;
    g_rng ^= g_rng >> 9;
    return (uint32_t)(g_rng & 0xFFFFFFFFu);
}

static void sink_discard(void *ctx, const char *d, size_t n) {
    (void)ctx; (void)d; (void)n;
}

static const RJsonStreamPathElem PATH[] = {
    {0, {.key = "choices"}}, {1, {.index = 0}}, {0, {.key = "delta"}},
    {0, {.key = "content"}}, {-2, {0}},
};

static void fuzz_json_stream(const uint8_t *buf, size_t n) {
    /* 分片喂入（随机切分点） */
    RJsonStream *st = rjson_stream_create(PATH, sink_discard, NULL);
    size_t off = 0;
    while (off < n) {
        size_t chunk = 1 + (xorshift() % 16);
        if (chunk > n - off) chunk = n - off;
        rjson_stream_feed(st, (const char *)buf + off, chunk);
        off += chunk;
    }
    rjson_stream_finish(st);
    rjson_stream_destroy(st);
}

static void fuzz_md(const uint8_t *buf, size_t n) {
    RikkaMdParser *p = rmd_create();
    /* 随机分块 feed（模拟流式畸形输入） */
    size_t off = 0;
    while (off < n) {
        size_t chunk = 1 + (xorshift() % 32);
        if (chunk > n - off) chunk = n - off;
        rmd_feed(p, (const char *)buf + off, chunk);
        off += chunk;
    }
    size_t cnt = 0;
    rmd_blocks(p, &cnt);
    rmd_destroy(p);
}

static void fuzz_rbin(const uint8_t *buf, size_t n) {
    /* 构造合法头 + 随机体 */
    uint8_t hdr[16];
    memcpy(hdr, "RIKKABIN", 8);
    hdr[8] = 1; hdr[9] = 0; hdr[10] = 0; hdr[11] = 0;       /* version=1 */
    hdr[12] = (uint8_t)(n % 100); hdr[13] = 0; hdr[14] = 0; hdr[15] = 0; /* count */
    size_t total = n + 16;
    uint8_t *full = (uint8_t *)malloc(total ? total : 1);
    memcpy(full, hdr, 16);
    memcpy(full + 16, buf, n);
    Arena *a = arena_create(0);
    RikkaMessage **msgs = NULL;
    size_t cnt = 0;
    rbin_load(full, total, a, &msgs, &cnt); /* 返回值忽略：只验证不崩 */
    arena_destroy(a);
    free(full);
}

int main(int argc, char **argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 10000;
    for (int r = 0; r < rounds; r++) {
        size_t n = xorshift() % 256;
        uint8_t buf[256];
        for (size_t i = 0; i < n; i++) buf[i] = (uint8_t)(xorshift() & 0xFF);
        fuzz_json_stream(buf, n);
        fuzz_md(buf, n);
        fuzz_rbin(buf, n);
        if (r && r % 5000 == 0) fprintf(stderr, "[fuzz] %d rounds ok\n", r);
    }
    printf("fuzz: %d rounds passed (json_stream / md / rbin)\n", rounds);
    return 0;
}
