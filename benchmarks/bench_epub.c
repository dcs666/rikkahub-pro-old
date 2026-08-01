/*
 * P3 基准：EPUB 解析（zip 中央目录 + DEFLATE 解压 + XHTML 文本提取）。
 * 数据用 zlib compress2/crc32 在 C 内构造合法 zip，零外部依赖。
 */
#define _POSIX_C_SOURCE 200809L
#include "rikka/doc/epub.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <zlib.h>

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

#define NCH 40
#define CHSIZE 2048

/* zip 的 method 8 = raw deflate (RFC1951)；compress2 是 zlib 包裹格式，
 * 必须用 deflateInit2(-15) 生成裸流。 */
static int raw_deflate(const char *src, uLong slen, unsigned char *dst, uLong *dlen) {
    z_stream s;
    memset(&s, 0, sizeof(s));
    if (deflateInit2(&s, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return -1;
    s.next_in = (Bytef *)src;
    s.avail_in = slen;
    s.next_out = dst;
    s.avail_out = *dlen;
    int rc = deflate(&s, Z_FINISH);
    *dlen = s.total_out;
    deflateEnd(&s);
    return rc == Z_STREAM_END ? 0 : -1;
}

static void put32(unsigned char *p, unsigned long v) {
    p[0] = (unsigned char)(v & 0xff); p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff); p[3] = (unsigned char)((v >> 24) & 0xff);
}
static void put16(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v & 0xff); p[1] = (unsigned char)((v >> 8) & 0xff);
}

/* 构造 NCH 个 DEFLATE 条目的最小 zip；返回总长，0 = 失败 */
static size_t build_zip(unsigned char *out, size_t cap) {
    char chapter[CHSIZE];
    unsigned char cbuf[CHSIZE * 2];
    unsigned crcs[NCH];
    uLong csizes[NCH], usizes[NCH];
    size_t lho[NCH], off = 0;
    for (int i = 0; i < NCH; i++) {
        int n = snprintf(chapter, sizeof(chapter),
            "<?xml version=\"1.0\"?><html><body><h1>Chapter %d</h1>"
            "<p>RikkaHub EPUB benchmark paragraph with some text content.</p></body></html>", i);
        if (n <= 0 || (size_t)n >= sizeof(chapter)) return 0;
        uLong ulen = (uLong)n, clen = (uLong)sizeof(cbuf);
        if (raw_deflate(chapter, ulen, cbuf, &clen) != 0) return 0;
        crcs[i] = (unsigned)crc32(0L, (const Bytef *)chapter, ulen);
        csizes[i] = clen; usizes[i] = ulen;
        char name[32];
        int nl = snprintf(name, sizeof(name), "OEBPS/ch%d.xhtml", i);
        lho[i] = off;
        if (off + 30 + (size_t)nl + clen > cap) return 0;
        unsigned char *p = out + off;
        put32(p, 0x04034b50); put16(p + 4, 20); put16(p + 6, 0);
        put16(p + 8, 8); put16(p + 10, 0); put16(p + 12, 0);
        put32(p + 14, crcs[i]); put32(p + 18, clen); put32(p + 22, ulen);
        put16(p + 26, (unsigned)nl); put16(p + 28, 0);
        memcpy(p + 30, name, (size_t)nl);
        memcpy(p + 30 + nl, cbuf, clen);
        off += 30 + (size_t)nl + clen;
    }
    size_t cd_off = off;
    for (int i = 0; i < NCH; i++) {
        char name[32];
        int nl = snprintf(name, sizeof(name), "OEBPS/ch%d.xhtml", i);
        if (off + 46 + (size_t)nl > cap) return 0;
        unsigned char *p = out + off;
        put32(p, 0x02014b50); put16(p + 4, 20); put16(p + 6, 20);
        put16(p + 8, 0); put16(p + 10, 8); put16(p + 12, 0); put16(p + 14, 0);
        put32(p + 16, crcs[i]); put32(p + 20, csizes[i]); put32(p + 24, usizes[i]);
        put16(p + 28, (unsigned)nl); put16(p + 30, 0); put16(p + 32, 0);
        put16(p + 34, 0); put16(p + 36, 0); put32(p + 38, 0); put32(p + 42, (unsigned long)lho[i]);
        memcpy(p + 46, name, (size_t)nl);
        off += 46 + (size_t)nl;
    }
    size_t cd_size = off - cd_off;
    if (off + 22 > cap) return 0;
    unsigned char *p = out + off;
    put32(p, 0x06054b50); put16(p + 4, 0); put16(p + 6, 0);
    put16(p + 8, NCH); put16(p + 10, NCH);
    put32(p + 12, (unsigned long)cd_size); put32(p + 16, (unsigned long)cd_off);
    put16(p + 20, 0);
    off += 22;
    return off;
}

int main(void) {
    static unsigned char zip[8 * 1024 * 1024];
    size_t zlen = build_zip(zip, sizeof(zip));
    if (zlen == 0) return 1;

    /* 正确性冒烟：必须解析出文本且包含第一章 */
    EpubContent c;
    if (epub_parse(zip, zlen, &c) != 0) return 2;
    if (!c.text || strstr(c.text, "Chapter 0") == NULL) return 3;
    size_t txtlen = c.len;
    epub_content_free(&c);

    /* 计时：20 次完整解析 */
    double t0 = now_sec();
    for (int i = 0; i < 20; i++) {
        if (epub_parse(zip, zlen, &c) != 0) return 4;
        epub_content_free(&c);
    }
    double t1 = now_sec();
    double ms = (t1 - t0) * 1e3 / 20;

    printf("epub (%zu B zip, %d chapters, %.1f KB text):\n", zlen, NCH, txtlen / 1e3);
    printf("  parse : %8.2f ms\n", ms);
    return 0;
}
