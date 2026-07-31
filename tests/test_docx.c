#include "test.h"
#include "rikka/doc/docx.h"
#include <stdio.h>
#include <string.h>

TEST(docx_parse_basic) {
    /* python 生成测试 docx 到 /tmp */
    int sysrc = system("python3 -c \"import zipfile; zf=zipfile.ZipFile('/tmp/test_rikka.docx','w',zipfile.ZIP_DEFLATED); zf.writestr('word/document.xml','<?xml version=\\\"1.0\\\"?><w:document xmlns:w=\\\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\\\"><w:body><w:p><w:r><w:t>Hello</w:t></w:r></w:p><w:p><w:r><w:t>World</w:t></w:r></w:p></w:body></w:document>'); zf.close()\"");
    ASSERT_EQ_INT(0, sysrc);
    FILE *f = fopen("/tmp/test_rikka.docx", "rb");
    ASSERT_NOT_NULL(f);
    unsigned char data[8192];
    size_t n = fread(data, 1, sizeof(data), f);
    fclose(f);
    ASSERT(n > 0);
    DocxContent out;
    int rc = docx_parse(data, n, &out);
    ASSERT_EQ_INT(0, rc);
    ASSERT_NOT_NULL(out.text);
    ASSERT(strstr(out.text, "Hello") != NULL);
    ASSERT(strstr(out.text, "World") != NULL);
    docx_content_free(&out);
}

TEST(docx_parse_invalid) {
    DocxContent out;
    int rc = docx_parse((const unsigned char *)"not a zip", 9, &out);
    ASSERT(rc != 0);
}

int run_docx_suite(void) {
    const RikkaTest tests[] = {
        RIKKA_TEST_REGISTER(docx, docx_parse_basic),
        RIKKA_TEST_REGISTER(docx, docx_parse_invalid),
    };
    return run_suite("docx", tests, sizeof(tests) / sizeof(tests[0]));
}
