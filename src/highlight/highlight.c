#include "rikka/highlight/highlight.h"
#include <ctype.h>
#include <string.h>

/* ---------- 关键字表 ---------- */

static const char *KW_C[] = {
    "auto","break","case","const","continue","default","do","else",
    "enum","extern","for","goto","if","inline","register",
    "restrict","return","sizeof","static","struct","switch","typedef",
    "union","volatile","while","_Complex", NULL};
static const char *TY_C[] = {
    "int","char","void","float","double","long","short","unsigned","signed","_Bool",
    "bool","int8_t","int16_t","int32_t","int64_t","uint8_t","uint16_t","uint32_t",
    "uint64_t","size_t","ssize_t","intptr_t","uintptr_t","FILE","errno_t", NULL};
static const char *BU_C[] = {
    "printf","scanf","fprintf","sprintf","snprintf","malloc","calloc","realloc","free",
    "memcpy","memset","memmove","strlen","strcmp","strncmp","strcpy","strncpy","strcat",
    "strstr","strchr","exit","abort","assert","perror","NULL","true","false", NULL};

static const char *KW_CPP[] = {
    "alignas","alignof","and","asm","bitand","bitor","catch","class","compl","concept",
    "constexpr","consteval","constinit","co_await","co_return","co_yield","decltype",
    "delete","explicit","export","friend","mutable","namespace","new","noexcept","not",
    "operator","or","private","protected","public","reinterpret_cast","requires",
    "static_assert","static_cast","template","this","thread_local","throw","try",
    "typeid","typename","using","virtual","xor","xor_eq", NULL};
static const char *TY_CPP[] = {
    "int","char","void","float","double","long","short","unsigned","signed","bool",
    "string","vector","map","set","unordered_map","unique_ptr","shared_ptr","weak_ptr",
    "optional","variant","any","pair","tuple","array","deque","list","stack","queue",
    "iostream","ostream","istream","fstream","string_view","nullptr_t", NULL};

static const char *KW_PY[] = {
    "False","None","True","and","as","assert","async","await","break","class","continue",
    "def","del","elif","else","except","finally","for","from","global","if","import",
    "in","is","lambda","nonlocal","not","or","pass","raise","return","try","while",
    "with","yield", NULL};
static const char *BU_PY[] = {
    "print","len","range","str","int","float","list","dict","set","tuple","map","filter",
    "zip","sorted","enumerate","open","input","type","isinstance","abs","min","max","sum",
    "any","all","reversed","format","repr","bytes","bool", NULL};

static const char *KW_JS[] = {
    "async","await","break","case","catch","class","const","continue","debugger","default",
    "delete","do","else","export","extends","finally","for","function","if","import","in",
    "instanceof","let","new","of","return","static","super","switch","this","throw","try",
    "typeof","var","void","while","with","yield","null","undefined", NULL};
static const char *BU_JS[] = {
    "console","document","window","JSON","Math","Date","Promise","Array","Object",
    "String","Number","Boolean","parseInt","parseFloat","setTimeout","setInterval",
    "fetch","Symbol","RegExp","Map","Set","WeakMap", NULL};
static const char *TY_TS[] = {"string","number","boolean","any","unknown","never","void",
    "object","Record","Partial","Required","Readonly","Pick","Omit", NULL};

static const char *KW_JAVA[] = {
    "abstract","assert","break","case","catch","class","const",
    "continue","default","do","else","enum","extends","final","finally",
    "for","goto","if","implements","import","instanceof","interface",
    "native","new","package","private","protected","public","return","static",
    "strictfp","super","switch","synchronized","this","throw","throws","transient","try",
    "volatile","while","true","false","null", NULL};
static const char *TY_JAVA[] = {
    "int","char","void","float","double","long","short","byte","boolean",
    "String","Integer","Long","Double","Float","Boolean","Character","Object","List",
    "ArrayList","Map","HashMap","Set","HashSet","Optional","Stream","Exception",
    "RuntimeException", NULL};

static const char *KW_KT[] = {
    "as","break","class","continue","do","else","false","for","fun","if","in","interface",
    "is","null","object","package","return","super","this","throw","true","try","typealias",
    "typeof","val","var","when","while","by","catch","finally","dynamic","field","file",
    "finally","get","import","init","param","property","receiver","set","setparam",
    "where", NULL};
static const char *TY_KT[] = {
    "String","Int","Long","Double","Float","Boolean","Char","Byte","Short","Unit",
    "Any","Nothing","List","MutableList","Map","Set","Array","Pair","Triple", NULL};
static const char *BU_KT[] = {"println","print","listOf","mapOf","setOf","arrayOf",
    "require","check","error","TODO", NULL};

static const char *KW_GO[] = {
    "break","case","chan","const","continue","default","defer","else","fallthrough",
    "for","func","go","goto","if","import","interface","map","package","range","return",
    "select","struct","switch","type","var", NULL};
static const char *TY_GO[] = {
    "string","int","int8","int16","int32","int64","uint","uint8","uint16","uint32",
    "uint64","uintptr","byte","rune","float32","float64","complex64","complex128",
    "bool","error","any", NULL};
static const char *BU_GO[] = {
    "make","new","len","cap","append","copy","delete","close","panic","recover","print",
    "println","fmt","nil","true","false", NULL};

static const char *KW_RS[] = {
    "as","async","await","break","const","continue","crate","dyn","else","enum","extern",
    "false","fn","for","if","impl","in","let","loop","match","mod","move","mut","pub",
    "ref","return","self","Self","static","struct","super","trait","true","type","unsafe",
    "use","where","while", NULL};
static const char *TY_RS[] = {
    "i8","i16","i32","i64","i128","isize","u8","u16","u32","u64","u128","usize",
    "f32","f64","bool","char","str","String","Vec","Option","Result","Box","HashMap",
    "HashSet","Rc","Arc","Mutex", NULL};
static const char *BU_RS[] = {"println","print","eprintln","format","vec","Some","None",
    "Ok","Err","panic","assert","assert_eq", NULL};

static const char *KW_SQL[] = {
    "SELECT","FROM","WHERE","INSERT","INTO","UPDATE","DELETE","CREATE","TABLE","DROP",
    "ALTER","JOIN","LEFT","RIGHT","INNER","OUTER","FULL","CROSS","ON","GROUP","BY",
    "ORDER","HAVING","LIMIT","OFFSET","AS","AND","OR","NOT","NULL","PRIMARY","KEY",
    "FOREIGN","INDEX","VIEW","BEGIN","COMMIT","ROLLBACK","UNION","ALL","DISTINCT",
    "VALUES","SET","CASE","WHEN","THEN","ELSE","END","EXISTS","BETWEEN","LIKE","IN",
    "IS","ASC","DESC","COUNT","SUM","AVG","MIN","MAX", NULL};

static const char *KW_BASH[] = {
    "if","then","else","elif","fi","for","while","do","done","case","esac","function",
    "in","select","until","return","break","continue","local","export","readonly",
    "declare","echo","cd","exit","source","trap","set","unset","shift", NULL};
static const char *BU_BASH[] = {"true","false", NULL};

static const char *KW_JSON[] = {"true","false","null", NULL};

typedef struct {
    const char *name;
    const char *const *keywords;
    const char *const *types;
    const char *const *builtins;
    int hash_comment;    /* # 行注释 */
    int dash_comment;    /* -- 行注释 */
    int preproc;         /* #include/#define（C 系） */
    int backtick_string; /* `...` */
    int html;            /* HTML 模式 */
    const char *line_comment; /* "//" 或 NULL */
    int block_comment;   /* /* *​/ */
} HlLangCfg;

static const HlLangCfg LANGS[] = {
    {"c",      KW_C, TY_C, BU_C, 0, 0, 1, 0, 0, "//", 1},
    {"cpp",    KW_CPP, TY_CPP, BU_C, 0, 0, 1, 0, 0, "//", 1},
    {"c++",    KW_CPP, TY_CPP, BU_C, 0, 0, 1, 0, 0, "//", 1},
    {"python", KW_PY, NULL, BU_PY, 1, 0, 0, 0, 0, NULL, 0},
    {"py",     KW_PY, NULL, BU_PY, 1, 0, 0, 0, 0, NULL, 0},
    {"js",     KW_JS, NULL, BU_JS, 0, 0, 0, 1, 0, "//", 1},
    {"javascript", KW_JS, NULL, BU_JS, 0, 0, 0, 1, 0, "//", 1},
    {"ts",     KW_JS, TY_TS, BU_JS, 0, 0, 0, 1, 0, "//", 1},
    {"typescript", KW_JS, TY_TS, BU_JS, 0, 0, 0, 1, 0, "//", 1},
    {"java",   KW_JAVA, TY_JAVA, NULL, 0, 0, 0, 0, 0, "//", 1},
    {"kotlin", KW_KT, TY_KT, BU_KT, 0, 0, 0, 0, 0, "//", 1},
    {"go",     KW_GO, TY_GO, BU_GO, 0, 0, 0, 1, 0, "//", 1},
    {"rust",   KW_RS, TY_RS, BU_RS, 0, 0, 0, 0, 0, "//", 1},
    {"sql",    KW_SQL, NULL, NULL, 0, 1, 0, 0, 0, NULL, 0},
    {"bash",   KW_BASH, NULL, BU_BASH, 1, 0, 0, 0, 0, NULL, 0},
    {"sh",     KW_BASH, NULL, BU_BASH, 1, 0, 0, 0, 0, NULL, 0},
    {"json",   KW_JSON, NULL, NULL, 0, 0, 0, 0, 0, NULL, 0},
    {"html",   NULL, NULL, NULL, 0, 0, 0, 0, 1, NULL, 0},
    {"xml",    NULL, NULL, NULL, 0, 0, 0, 0, 1, NULL, 0},
    {"css",    NULL, NULL, NULL, 0, 0, 0, 0, 0, NULL, 1},
};

size_t rikka_hl_lang_count(void) { return sizeof(LANGS) / sizeof(LANGS[0]); }
const char *rikka_hl_lang_name(size_t idx) {
    if (idx >= sizeof(LANGS) / sizeof(LANGS[0])) return NULL;
    return LANGS[idx].name;
}

static const HlLangCfg *find_lang(const char *lang) {
    if (!lang) return NULL;
    for (size_t i = 0; i < sizeof(LANGS) / sizeof(LANGS[0]); i++) {
        if (strcmp(LANGS[i].name, lang) == 0) return &LANGS[i];
    }
    return NULL;
}

static int in_table(const char *const *tbl, const char *word, size_t len) {
    if (!tbl) return 0;
    for (; *tbl; tbl++) {
        if (strlen(*tbl) == len && memcmp(*tbl, word, len) == 0) return 1;
    }
    return 0;
}

static size_t push(RikkaHlToken *out, size_t cap, size_t *n,
                   size_t start, size_t len, RikkaHlType type) {
    if (*n >= cap) return 0;
    out[*n].start = start;
    out[*n].len = len;
    out[*n].type = type;
    (*n)++;
    return 1;
}

static size_t scan_string(const char *code, size_t len, size_t i, char quote) {
    i++;
    while (i < len) {
        if (code[i] == '\\' && i + 1 < len) { i += 2; continue; }
        if (code[i] == quote) { i++; break; }
        i++;
    }
    return i;
}

/* 标识符扫描 + 分类 */
static void lex_ident(const HlLangCfg *cfg, const char *code, size_t len,
                      size_t *i, RikkaHlToken *out, size_t cap, size_t *n) {
    size_t start = *i;
    while (*i < len && (isalnum((unsigned char)code[*i]) || code[*i] == '_'))
        (*i)++;
    size_t wlen = *i - start;
    const char *w = code + start;
    RikkaHlType t = RIKKA_HL_PLAIN;
    if (in_table(cfg->types, w, wlen)) t = RIKKA_HL_TYPE;          /* 类型优先 */
    else if (in_table(cfg->keywords, w, wlen)) t = RIKKA_HL_KEYWORD;
    else if (in_table(cfg->builtins, w, wlen)) t = RIKKA_HL_BUILTIN;
    else {
        /* 函数调用：后跟 ( 且前一个 token 是普通标识符 */
        size_t j = *i;
        while (j < len && (code[j] == ' ' || code[j] == '\t')) j++;
        if (j < len && code[j] == '(') t = RIKKA_HL_FUNC;
    }
    push(out, cap, n, start, wlen, t);
}

/* HTML 模式 */
static void lex_html(const char *code, size_t len, size_t *i,
                     RikkaHlToken *out, size_t cap, size_t *n) {
    if (code[*i] == '<') {
        /* 注释 */
        if (len - *i >= 4 && memcmp(code + *i, "<!--", 4) == 0) {
            size_t start = *i;
            const char *end = strstr(code + *i + 4, "-->");
            *i = end ? (size_t)(end - code) + 3 : len;
            push(out, cap, n, start, *i - start, RIKKA_HL_COMMENT);
            return;
        }
        size_t start = *i;
        (*i)++; /* < */
        int closing = 0;
        if (*i < len && code[*i] == '/') { closing = 1; (*i)++; }
        size_t tstart = *i;
        while (*i < len && (isalnum((unsigned char)code[*i]) || code[*i] == '-')) (*i)++;
        push(out, cap, n, start, *i - start, RIKKA_HL_TAG);
        (void)closing; (void)tstart;
        /* 属性与值 */
        while (*i < len && code[*i] != '>') {
            if (code[*i] == ' ' || code[*i] == '\t' || code[*i] == '\n' || code[*i] == '\r' ||
                code[*i] == '/' || code[*i] == '=') {
                (*i)++;
                continue;
            }
            if (code[*i] == '"' || code[*i] == '\'') {
                size_t s = *i;
                *i = scan_string(code, len, *i, code[*i]);
                push(out, cap, n, s, *i - s, RIKKA_HL_STRING);
                continue;
            }
            size_t astart = *i;
            while (*i < len && (isalnum((unsigned char)code[*i]) || code[*i] == '-' ||
                                code[*i] == '_' || code[*i] == ':'))
                (*i)++;
            if (*i > astart)
                push(out, cap, n, astart, *i - astart, RIKKA_HL_ATTR);
            else
                (*i)++;
        }
        if (*i < len) (*i)++; /* > */
        return;
    }
    /* 文本 */
    size_t start = *i;
    while (*i < len && code[*i] != '<') (*i)++;
    push(out, cap, n, start, *i - start, RIKKA_HL_PLAIN);
}

/* CSS 模式 */
static void lex_css(const char *code, size_t len, size_t *i,
                    RikkaHlToken *out, size_t cap, size_t *n) {
    size_t start = *i;
    char c = code[*i];
    if (c == '/' && *i + 1 < len && code[*i + 1] == '*') {
        const char *end = strstr(code + *i + 2, "*/");
        *i = end ? (size_t)(end - code) + 2 : len;
        push(out, cap, n, start, *i - start, RIKKA_HL_COMMENT);
        return;
    }
    if (c == '"' || c == '\'') {
        *i = scan_string(code, len, *i, c);
        push(out, cap, n, start, *i - start, RIKKA_HL_STRING);
        return;
    }
    if (isalpha((unsigned char)c) || c == '_' || c == '-') {
        size_t k = *i;
        while (k < len && (isalnum((unsigned char)code[k]) || code[k] == '-' || code[k] == '_')) k++;
        /* 冒号前的属性名 → ATTR */
        size_t j = k;
        while (j < len && code[j] == ' ') j++;
        RikkaHlType t = (j < len && code[j] == ':') ? RIKKA_HL_ATTR : RIKKA_HL_PLAIN;
        *i = k;
        push(out, cap, n, start, k - start, t);
        return;
    }
    if (isdigit((unsigned char)c) || (c == '.' && *i + 1 < len && isdigit((unsigned char)code[*i + 1]))) {
        while (*i < len && (isalnum((unsigned char)code[*i]) || code[*i] == '.' ||
                            code[*i] == '%' || code[*i] == '#' || code[*i] == '-'))
            (*i)++;
        push(out, cap, n, start, *i - start, RIKKA_HL_NUMBER);
        return;
    }
    (*i)++;
    push(out, cap, n, start, 1, RIKKA_HL_OPERATOR);
}

size_t rikka_hl_tokenize(const char *lang, const char *code, size_t len,
                         RikkaHlToken *out, size_t cap) {
    const HlLangCfg *cfg = find_lang(lang);
    if (!cfg || cap == 0) {
        if (out && cap > 0 && code) { out[0].start = 0; out[0].len = len; out[0].type = RIKKA_HL_PLAIN; return 1; }
        return 0;
    }
    size_t n = 0, i = 0;
    while (i < len && n < cap) {
        char c = code[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { i++; continue; }

        if (cfg->html) { lex_html(code, len, &i, out, cap, &n); continue; }
        if (cfg->name[0] == 'c' && cfg->name[1] == 's' && cfg->name[2] == 's') {
            lex_css(code, len, &i, out, cap, &n);
            continue;
        }

        /* 注释 */
        if (cfg->line_comment && len - i >= 2 && code[i] == '/' && code[i + 1] == '/') {
            size_t start = i;
            while (i < len && code[i] != '\n') i++;
            push(out, cap, &n, start, i - start, RIKKA_HL_COMMENT);
            continue;
        }
        if (cfg->block_comment && len - i >= 2 && code[i] == '/' && code[i + 1] == '*') {
            size_t start = i;
            const char *end = strstr(code + i + 2, "*/");
            i = end ? (size_t)(end - code) + 2 : len;
            push(out, cap, &n, start, i - start, RIKKA_HL_COMMENT);
            continue;
        }
        if (cfg->hash_comment && c == '#') {
            size_t start = i;
            while (i < len && code[i] != '\n') i++;
            push(out, cap, &n, start, i - start, RIKKA_HL_COMMENT);
            continue;
        }
        if (cfg->dash_comment && len - i >= 2 && code[i] == '-' && code[i + 1] == '-') {
            size_t start = i;
            while (i < len && code[i] != '\n') i++;
            push(out, cap, &n, start, i - start, RIKKA_HL_COMMENT);
            continue;
        }
        if (cfg->preproc && c == '#') {
            size_t start = i;
            while (i < len && code[i] != '\n') i++;
            push(out, cap, &n, start, i - start, RIKKA_HL_PREPROC);
            continue;
        }

        /* 字符串 */
        if (c == '"' || c == '\'') {
            size_t start = i;
            i = scan_string(code, len, i, c);
            push(out, cap, &n, start, i - start, RIKKA_HL_STRING);
            continue;
        }
        if (cfg->backtick_string && c == '`') {
            size_t start = i;
            i = scan_string(code, len, i, '`');
            push(out, cap, &n, start, i - start, RIKKA_HL_STRING);
            continue;
        }

        /* 数字 */
        if (isdigit((unsigned char)c) ||
            (c == '.' && i + 1 < len && isdigit((unsigned char)code[i + 1]))) {
            size_t start = i;
            while (i < len) {
                char d = code[i];
                if (isalnum((unsigned char)d) || d == '.' || d == '_' ||
                    d == '+' || d == '-' || d == 'x' || d == 'X') {
                    /* 避免把 a-b 的 - 吞进数字 */
                    if ((d == '+' || d == '-') && !(i > start &&
                        (code[i-1] == 'e' || code[i-1] == 'E')))
                        break;
                    i++;
                } else break;
            }
            push(out, cap, &n, start, i - start, RIKKA_HL_NUMBER);
            continue;
        }

        /* 标识符 */
        if (isalpha((unsigned char)c) || c == '_') {
            lex_ident(cfg, code, len, &i, out, cap, &n);
            continue;
        }

        /* 其他符号 */
        size_t start = i;
        i++;
        push(out, cap, &n, start, 1, RIKKA_HL_OPERATOR);
    }
    return n;
}
