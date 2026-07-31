#include "test.h"
#include "rikka/doc/epub.h"
#include <stdio.h>
#include <string.h>

TEST(epub_parse_basic) {
    /* python 生成测试 epub 到 /tmp */
    int sysrc = system("python3 -c \"import zipfile; zf=zipfile.ZipFile('/tmp/test_rikka.epub','w',zipfile.ZIP_DEFLATED); zf.writestr('mimetype','application/epub+zip'); zf.writestr('OEBPS/ch1.xhtml','<?xml version=\\\"1.0\\\"?><html><body><h1>Chapter 1</h1><p>Hello EPUB</p></body></html>'); zf.writestr('OEBPS/ch2.xhtml','<?xml version=\\\"1.0\\\"?><html><body><h1>Chapter 2</h1><p>World EPUB</p></body></html>'); zf.close()\"");
    ASSERT_EQ_INT(0, sysrc);
    FILE *f = fopen("/tmp/test_rikka.epub", "rb");
    ASSERT_NOT_NULL(f);
    unsigned char data[16384];
    size_t n = fread(data, 1, sizeof(data), f);
    fclose(f);
    ASSERT(n > 0);
    EpubContent out;
    int rc = epub_parse(data, n, &out);
    ASSERT_EQ_INT(0, rc);
    ASSERT_NOT_NULL(out.text);
    ASSERT(strstr(out.text, "Chapter 1") != NULL);
    ASSERT(strstr(out.text, "Hello EPUB") != NULL);
    ASSERT(strstr(out.text, "Chapter 2") != NULL);
    ASSERT(strstr(out.text, "World EPUB") != NULL);
    epub_content_free(&out);
}

TEST(epub_parse_invalid) {
    EpubContent out;
    int rc = epub_parse((const unsigned char *)"not a zip", 9, &out);
    ASSERT(rc != 0);
}

int run_epub_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(epub, epub_parse_basic),
        RIKKA_TEST_REGISTER(epub, epub_parse_invalid),
    };
    return run_suite("epub", tests, sizeof(tests) / sizeof(tests[0]));
}
