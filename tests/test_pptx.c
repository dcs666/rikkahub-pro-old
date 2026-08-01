#define _DEFAULT_SOURCE
#include "test.h"
#include "rikka/doc/pptx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* 用 python zipfile 生成最小 pptx（与 test_epub 同模式） */
static int make_pptx(const char *path) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "python3 -c \"import zipfile;"
        " zf=zipfile.ZipFile('%s','w',zipfile.ZIP_DEFLATED);"
        " zf.writestr('ppt/slides/slide1.xml','<?xml version=\\\"1.0\\\"?><p:sld xmlns:a=\\\"x\\\"><a:t>Slide One Title</a:t><a:p/><a:t>Body &amp; text</a:t></p:sld>');"
        " zf.writestr('ppt/slides/slide2.xml','<?xml version=\\\"1.0\\\"?><p:sld xmlns:a=\\\"x\\\"><a:t>Slide Two</a:t></p:sld>');"
        " zf.writestr('ppt/presentation.xml','<p:presentation/>');"
        " zf.close()\"", path);
    return system(cmd);
}

TEST(pptx_parse_basic) {
    const char *path = "/tmp/test_rikka.pptx";
    ASSERT_EQ_INT(0, make_pptx(path));
    FILE *f = fopen(path, "rb");
    ASSERT_NOT_NULL(f);
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    ASSERT(flen > 0);
    unsigned char *data = (unsigned char *)malloc((size_t)flen);
    ASSERT_NOT_NULL(data);
    ASSERT_EQ_SIZE((size_t)flen, fread(data, 1, (size_t)flen, f));
    fclose(f);

    PptxContent c;
    ASSERT_EQ_INT(0, pptx_parse(data, (size_t)flen, &c));
    ASSERT_NOT_NULL(c.text);
    /* 两张 slide 的文本 + 实体解码 + slide 分隔 */
    ASSERT(strstr(c.text, "Slide One Title") != NULL);
    ASSERT(strstr(c.text, "Body & text") != NULL);   /* &amp; 解码 */
    ASSERT(strstr(c.text, "Slide Two") != NULL);
    free(data);
    pptx_content_free(&c);
    unlink(path);
}

TEST(pptx_parse_bad_data) {
    PptxContent c;
    ASSERT_EQ_INT(-1, pptx_parse((const unsigned char *)"not a zip", 9, &c));
    ASSERT_NULL(c.text);
}

TEST(pptx_parse_no_slides) {
    /* 合法 zip 但没有 slide XML */
    const char *path = "/tmp/test_rikka_noslide.pptx";
    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "python3 -c \"import zipfile;"
        " zf=zipfile.ZipFile('%s','w',zipfile.ZIP_DEFLATED);"
        " zf.writestr('ppt/presentation.xml','<p:presentation/>');"
        " zf.close()\"", path);
    ASSERT_EQ_INT(0, system(cmd));
    FILE *f = fopen(path, "rb");
    ASSERT_NOT_NULL(f);
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = (unsigned char *)malloc((size_t)flen);
    ASSERT_NOT_NULL(data);
    ASSERT_EQ_SIZE((size_t)flen, fread(data, 1, (size_t)flen, f));
    fclose(f);
    PptxContent c;
    ASSERT_EQ_INT(-1, pptx_parse(data, (size_t)flen, &c));
    free(data);
    unlink(path);
}

int run_pptx_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(pptx, pptx_parse_basic),
        RIKKA_TEST_REGISTER(pptx, pptx_parse_bad_data),
        RIKKA_TEST_REGISTER(pptx, pptx_parse_no_slides),
    };
    return run_suite("pptx", tests, sizeof(tests) / sizeof(tests[0]));
}
