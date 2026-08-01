#define _POSIX_C_SOURCE 200809L
#include "rikka/doc/docx.h"
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

/* ZIP local file header signature */
#define ZIP_LOCAL_SIG 0x04034b50

/* ZIP central directory signatures */
#define ZIP_CENTRAL_SIG 0x02014b50
#define ZIP_EOCD_SIG 0x06054b50

/* 从 central directory 找文件（更可靠） */
static const uint8_t *zip_find_central(const uint8_t *data, size_t len,
                                        const char *name, size_t *comp_size,
                                        size_t *uncomp_size, int *method,
                                        size_t *local_offset) {
    /* 找 End of Central Directory Record（从末尾向前扫描） */
    size_t eocd_off = 0;
    if (len >= 22) {
        for (size_t i = len - 22; i > 0; i--) {
            if (i + 3 >= len) continue;
            uint32_t sig = data[i] | (data[i+1] << 8) | (data[i+2] << 16) | (data[i+3] << 24);
            if (sig == ZIP_EOCD_SIG) {
                eocd_off = i;
                break;
            }
        }
    }
    if (eocd_off == 0) return NULL;
    /* 解析 EOCD */
    uint16_t total_entries = data[eocd_off+10] | (data[eocd_off+11] << 8);
    uint32_t cd_offset = data[eocd_off+16] | (data[eocd_off+17] << 8) |
                         (data[eocd_off+18] << 16) | (data[eocd_off+19] << 24);
    if (cd_offset >= len) return NULL;
    /* 遍历 central directory */
    size_t off = cd_offset;
    size_t name_len = strlen(name);
    for (uint16_t i = 0; i < total_entries && off + 46 <= len; i++) {
        uint32_t sig = data[off] | (data[off+1] << 8) | (data[off+2] << 16) | (data[off+3] << 24);
        if (sig != ZIP_CENTRAL_SIG) break;
        uint16_t comp_method = data[off+10] | (data[off+11] << 8);
        uint32_t comp_sz = data[off+20] | (data[off+21] << 8) | (data[off+22] << 16) | (data[off+23] << 24);
        uint32_t uncomp_sz = data[off+24] | (data[off+25] << 8) | (data[off+26] << 16) | (data[off+27] << 24);
        uint16_t fname_len = data[off+28] | (data[off+29] << 8);
        uint16_t extra_len = data[off+30] | (data[off+31] << 8);
        uint16_t comment_len = data[off+32] | (data[off+33] << 8);
        uint32_t local_off = data[off+42] | (data[off+43] << 8) | (data[off+44] << 16) | (data[off+45] << 24);
        if (fname_len > len - off - 46) break; /* 溢出防护 */
        const char *fname = (const char *)data + off + 46;
        if (fname_len == name_len && memcmp(fname, name, name_len) == 0) {
            *comp_size = comp_sz;
            *uncomp_size = uncomp_sz;
            *method = comp_method;
            *local_offset = local_off;
            /* 从 local header 找数据偏移 */
            if (local_off + 30 <= len) {
                uint16_t l_fname_len = data[local_off+26] | (data[local_off+27] << 8);
                uint16_t l_extra_len = data[local_off+28] | (data[local_off+29] << 8);
                size_t data_off = local_off + 30 + l_fname_len + l_extra_len;
                if (data_off <= len) return data + data_off;
            }
            return NULL;
        }
        size_t next_off = off + 46 + fname_len + extra_len + comment_len;
        if (next_off <= off) break; /* 溢出防护 */
        off = next_off;
    }
    return NULL;
}

/* 从 zip 数据中找指定文件名，返回压缩数据指针 + 压缩大小 + 解压大小 */
static const uint8_t *zip_find_file(const uint8_t *data, size_t len,
                                     const char *name, size_t *comp_size,
                                     size_t *uncomp_size, int *method) {
    /* 优先用 central directory */
    size_t local_offset = 0;
    const uint8_t *result = zip_find_central(data, len, name, comp_size, uncomp_size, method, &local_offset);
    if (result) return result;
    /* 回退：扫描 local file header */
    size_t off = 0;
    size_t name_len = strlen(name);
    while (off + 30 <= len) {
        /* 检查 signature */
        uint32_t sig = data[off] | (data[off+1] << 8) | (data[off+2] << 16) | (data[off+3] << 24);
        if (sig != ZIP_LOCAL_SIG) break;
        /* 解析 header */
        uint16_t comp_method = data[off+8] | (data[off+9] << 8);
        uint32_t comp_sz = data[off+18] | (data[off+19] << 8) | (data[off+20] << 16) | (data[off+21] << 24);
        uint32_t uncomp_sz = data[off+22] | (data[off+23] << 8) | (data[off+24] << 16) | (data[off+25] << 24);
        uint16_t fname_len = data[off+26] | (data[off+27] << 8);
        uint16_t extra_len = data[off+28] | (data[off+29] << 8);
        if (fname_len > len - off - 30) break; /* 溢出防护 */
        const char *fname = (const char *)data + off + 30;
        size_t data_off = off + 30 + fname_len + extra_len;
        if (data_off > len) break;
        if (fname_len == name_len && memcmp(fname, name, name_len) == 0) {
            *comp_size = comp_sz;
            *uncomp_size = uncomp_sz;
            *method = comp_method;
            return data + data_off;
        }
        off = data_off + comp_sz;
    }
    return NULL;
}

/* XML 标签剥离：提取 <w:t>...</w:t> 之间的文本 */
static char *xml_extract_text(const char *xml, size_t len, size_t *out_len) {
    /* 预估输出大小（XML 文本通常 < 原始大小） */
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    size_t out_off = 0;
    size_t i = 0;
    while (i < len) {
        /* 找 <w:t> 或 <w:t ...> */
        if (xml[i] == '<' && i + 4 < len && memcmp(xml + i, "<w:t", 4) == 0) {
            /* 找 > */
            size_t tag_end = i + 4;
            while (tag_end < len && xml[tag_end] != '>') tag_end++;
            if (tag_end >= len) break;
            tag_end++; /* 跳过 > */
            /* 找 </w:t> */
            size_t text_start = tag_end;
            size_t close = text_start;
            while (close < len && len - close >= 6 && memcmp(xml + close, "</w:t>", 6) != 0) close++;
            if (close >= len || len - close < 6) break;
            /* 复制文本 */
            size_t text_len = close - text_start;
            if (text_len > len - out_off) break; /* 溢出防护 */
            memcpy(out + out_off, xml + text_start, text_len);
            out_off += text_len;
            /* 段落结束加换行 */
            if (out_off > 0 && out_off < len) out[out_off++] = '\n';
            i = close + 6;
        } else {
            i++;
        }
    }
    out[out_off] = '\0';
    *out_len = out_off;
    return out;
}

int docx_parse(const uint8_t *data, size_t len, DocxContent *out) {
    if (!data || !out) return -1;
    out->text = NULL;
    out->len = 0;
    /* 找 word/document.xml */
    size_t comp_size = 0, uncomp_size = 0;
    int method = 0;
    const uint8_t *comp_data = zip_find_file(data, len, "word/document.xml",
                                              &comp_size, &uncomp_size, &method);
    if (!comp_data) return -1;
    /* 解压 */
    char *xml = NULL;
    size_t xml_len = 0;
    if (method == 0) {
        /* 无压缩 */
        xml = (char *)malloc(comp_size + 1);
        if (!xml) return -1;
        memcpy(xml, comp_data, comp_size);
        xml[comp_size] = '\0';
        xml_len = comp_size;
    } else if (method == 8) {
        /* deflate */
        xml = (char *)malloc(uncomp_size + 1);
        if (!xml) return -1;
        z_stream strm;
        memset(&strm, 0, sizeof(strm));
        strm.next_in = (Bytef *)comp_data;
        strm.avail_in = comp_size;
        strm.next_out = (Bytef *)xml;
        strm.avail_out = uncomp_size;
        if (inflateInit2(&strm, -15) != Z_OK) { /* raw deflate */
            free(xml);
            return -1;
        }
        int ret = inflate(&strm, Z_FINISH);
        inflateEnd(&strm);
        if (ret != Z_STREAM_END && ret != Z_OK) {
            free(xml);
            return -1;
        }
        xml_len = strm.total_out;
        if (xml_len > uncomp_size) { /* 解压大小异常 */
            free(xml);
            return -1;
        }
        xml[xml_len] = '\0';
    } else {
        return -1; /* 不支持的压缩方法 */
    }
    /* XML 文本提取 */
    out->text = xml_extract_text(xml, xml_len, &out->len);
    free(xml);
    if (!out->text) return -1;
    return 0;
}

void docx_content_free(DocxContent *c) {
    if (c) {
        free(c->text);
        c->text = NULL;
        c->len = 0;
    }
}
