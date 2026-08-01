/*
 * MCP OAuth 2.1 授权客户端实现（见 mcp_oauth.h 的环节说明）。
 *
 * 依赖：rhttp（HTTP）、rjson（JSON）、OpenSSL SHA-256（PKCE S256）。
 * 令牌交换用 application/x-www-form-urlencoded（与 JVM 版一致）。
 */
#define _POSIX_C_SOURCE 200809L
#include "rikka/mcp/mcp_oauth.h"
#include "rikka/http/http.h"
#include "rikka/json/json.h"
#include "rikka/util/arena.h"
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void rk_mcp_oauth_free(RkMcpOAuth *o) {
    if (!o) return;
    free(o->resource);
    free(o->authorization_endpoint);
    free(o->token_endpoint);
    free(o->registration_endpoint);
    free(o->client_id);
    free(o->client_secret);
    free(o->access_token);
    free(o->refresh_token);
    free(o->scope);
    memset(o, 0, sizeof(*o));
}

/* ---------- 工具 ---------- */static const char B64URL_ALPHABET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

char *rk_oauth_b64url_encode(const uint8_t *data, size_t len) {
    size_t out_len = (len + 2) / 3 * 4;
    char *out = (char *)malloc(out_len + 1);
    if (!out) return NULL;
    size_t oi = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) v |= (uint32_t)data[i + 2];
        out[oi++] = B64URL_ALPHABET[(v >> 18) & 0x3f];
        out[oi++] = B64URL_ALPHABET[(v >> 12) & 0x3f];
        out[oi++] = i + 1 < len ? B64URL_ALPHABET[(v >> 6) & 0x3f] : '=';
        out[oi++] = i + 2 < len ? B64URL_ALPHABET[v & 0x3f] : '=';
    }
    /* 去 padding（base64url 无 padding） */
    size_t n = out_len;
    while (n > 0 && out[n - 1] == '=') n--;
    out[n] = '\0';
    return out;
}

static int b64val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

uint8_t *rk_oauth_b64url_decode(const char *s, size_t *len_out) {
    size_t slen = strlen(s);
    if (len_out) *len_out = 0;
    if (slen == 0) return NULL;
    size_t cap = slen / 4 * 3 + 3;
    uint8_t *out = (uint8_t *)malloc(cap);
    if (!out) return NULL;
    size_t oi = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < slen; i++) {
        if (s[i] == '=') break; /* padding 终止 */
        int v = b64val(s[i]);
        if (v < 0) { free(out); return NULL; }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[oi++] = (uint8_t)((acc >> bits) & 0xff);
        }
    }
    if (len_out) *len_out = oi;
    return out;
}

char *rk_oauth_urlencode(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *out = (char *)malloc(n * 3 + 1);
    if (!out) return NULL;
    size_t oi = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out[oi++] = (char)c;
        } else {
            int hi = (c >> 4) & 0xf;
            int lo = c & 0xf;
            out[oi++] = '%';
            out[oi++] = (char)(hi < 10 ? '0' + hi : 'A' + hi - 10);
            out[oi++] = (char)(lo < 10 ? '0' + lo : 'A' + lo - 10);
        }
    }
    out[oi] = '\0';
    return out;
}

/* ---------- HTTP 辅助 ---------- */

/* 同步请求：返回 malloc body（调用方 free）；status 输出；headers 传入可为 NULL */
static char *http_req(const char *url, const char *method, const char *const *headers,
                      const char *body, size_t body_len, int timeout_ms, int *status_out) {
    char host[256], path[512];
    uint16_t port;
    int tls;
    if (rhttp_parse_url(url, host, sizeof(host), &port, &tls, path, sizeof(path)) != 0)
        return NULL;
    RHttpConn *conn = rhttp_connect(host, port, tls, timeout_ms);
    if (!conn) return NULL;
    if (rhttp_send(conn, method, path, headers, body, body_len) != 0) {
        rhttp_close(conn);
        return NULL;
    }
    RHttpResp resp;
    if (rhttp_read_headers(conn, &resp, timeout_ms) != 0) {
        rhttp_close(conn);
        return NULL;
    }
    if (status_out) *status_out = resp.status;
    char *out = NULL;
    size_t out_len = 0;
    if (resp.content_length >= 0 && resp.content_length <= 65536) {
        out = (char *)malloc((size_t)resp.content_length + 1);
        if (out) {
            size_t want = (size_t)resp.content_length;
            while (out_len < want) {
                ssize_t r = rhttp_read_body(conn, out + out_len, want - out_len, timeout_ms);
                if (r <= 0) break;
                out_len += (size_t)r;
            }
            out[out_len] = '\0';
        }
    } else {
        /* 未知长度：读满 64KB 上限 */
        out = (char *)malloc(65537);
        if (out) {
            while (out_len < 65536) {
                ssize_t r = rhttp_read_body(conn, out + out_len, 65536 - out_len, timeout_ms);
                if (r <= 0) break;
                out_len += (size_t)r;
            }
            out[out_len] = '\0';
        }
    }
    rhttp_close(conn);
    return out;
}

/* JSON 字符串字段提取（arena 内）；不存在返回 NULL */
static const char *json_str_field(RJson *obj, const char *key) {
    const RJson *v = rjson_obj_get(obj, key);
    if (!v || v->type != RJSON_STRING) return NULL;
    return v->u.str.ptr;
}

/* ---------- 1. 元数据发现 ---------- */

static int fetch_resource_metadata(RkMcpOAuth *o, const char *meta_url, int timeout_ms) {
    int status = 0;
    char *body = http_req(meta_url, "GET", NULL, NULL, 0, timeout_ms, &status);
    if (!body || status < 200 || status >= 300) {
        free(body);
        return -1;
    }
    Arena *a = arena_create(0);
    size_t err = 0;
    RJson *v = rjson_parse(a, body, strlen(body), &err);
    free(body);
    int rc = -1;
    if (v) {
        const char *auth_servers[4];
        size_t n_as = 0;
        const RJson *servers = rjson_obj_get(v, "authorization_servers");
        if (servers && servers->type == RJSON_ARRAY) {
            for (size_t i = 0; i < servers->u.arr.count && n_as < 4; i++) {
                const RJson *e = servers->u.arr.items[i];
                if (e->type == RJSON_STRING) auth_servers[n_as++] = e->u.str.ptr;
            }
        }
        const char *res = json_str_field(v, "resource");
        if (res) {
            free(o->resource);
            o->resource = strdup(res);
        }
        /* 取第一个授权服务器做元数据发现 */
        if (n_as > 0) {
            char well_known[1024];
            snprintf(well_known, sizeof(well_known), "%s/.well-known/oauth-authorization-server",
                     auth_servers[0]);
            int st2 = 0;
            char *body2 = http_req(well_known, "GET", NULL, NULL, 0, timeout_ms, &st2);
            if (body2 && st2 >= 200 && st2 < 300) {
                Arena *a2 = arena_create(0);
                size_t err2 = 0;
                RJson *v2 = rjson_parse(a2, body2, strlen(body2), &err2);
                if (v2) {
                    const char *ae = json_str_field(v2, "authorization_endpoint");
                    const char *te = json_str_field(v2, "token_endpoint");
                    const char *re = json_str_field(v2, "registration_endpoint");
                    if (ae) { free(o->authorization_endpoint); o->authorization_endpoint = strdup(ae); }
                    if (te) { free(o->token_endpoint); o->token_endpoint = strdup(te); }
                    if (re) { free(o->registration_endpoint); o->registration_endpoint = strdup(re); }
                    if (o->authorization_endpoint && o->token_endpoint) rc = 0;
                }
                arena_destroy(a2);
            }
            free(body2);
        }
    }
    arena_destroy(a);
    return rc;
}

int rk_mcp_oauth_discover(RkMcpOAuth *o, const char *resource_url, int timeout_ms) {
    if (!o || !resource_url) return -1;
    /* 请求资源：期望 401/403 + WWW-Authenticate: resource_metadata="..." */
    char host[256], path[512];
    uint16_t port;
    int tls;
    if (rhttp_parse_url(resource_url, host, sizeof(host), &port, &tls, path, sizeof(path)) != 0)
        return -1;
    RHttpConn *conn = rhttp_connect(host, port, tls, timeout_ms);
    if (!conn) return -1;
    RHttpResp resp;
    if (rhttp_send(conn, "GET", path, NULL, NULL, 0) != 0 ||
        rhttp_read_headers(conn, &resp, timeout_ms) != 0) {
        rhttp_close(conn);
        return -1;
    }
    int status = resp.status;
    char meta_url[1024] = {0};
    int found = 0;
    if (status == 401 || status == 403) {
        char wa[1024];
        if (rhttp_resp_header(conn, "WWW-Authenticate", wa, sizeof(wa)) == 0) {
            /* 解析 resource_metadata="URL" */
            const char *p = strstr(wa, "resource_metadata");
            if (p) {
                p = strchr(p, '"');
                if (p) {
                    p++;
                    const char *q = strchr(p, '"');
                    if (q && (size_t)(q - p) < sizeof(meta_url)) {
                        memcpy(meta_url, p, (size_t)(q - p));
                        meta_url[q - p] = '\0';
                        found = 1;
                    }
                }
            }
        }
    }
    rhttp_close(conn);
    if (!found) {
        /* 退回 well-known 路径 */
        snprintf(meta_url, sizeof(meta_url), "%s/.well-known/mcp-resource-metadata",
                 resource_url);
    }
    return fetch_resource_metadata(o, meta_url, timeout_ms);
}

/* ---------- 2. 动态客户端注册 ---------- */

int rk_mcp_oauth_register(RkMcpOAuth *o, const char *redirect_uri,
                          const char *scope, int timeout_ms) {
    if (!o || !o->registration_endpoint || !redirect_uri) return -1;
    /* JSON body（redirect_uri/scope 直接内嵌；文档约定：不得含引号） */
    char body[2048];
    int n = snprintf(body, sizeof(body),
                     "{\"client_name\":\"rikkahub\",\"redirect_uris\":[\"%s\"],"
                     "\"grant_types\":[\"authorization_code\",\"refresh_token\"],"
                     "\"response_types\":[\"code\"],"
                     "\"token_endpoint_auth_method\":\"none\"%s%s%s}",
                     redirect_uri,
                     scope ? ", \"scope\": \"" : "",
                     scope ? scope : "",
                     scope ? "\"" : "");
    if (n < 0 || (size_t)n >= sizeof(body)) return -1;
    const char *hdrs[] = {
        "Content-Type", "application/json",
        "Accept", "application/json",
        NULL
    };
    int status = 0;
    char *resp = http_req(o->registration_endpoint, "POST", hdrs, body, strlen(body),
                          timeout_ms, &status);
    if (!resp || status < 200 || status >= 300) {
        free(resp);
        return -1;
    }
    Arena *a = arena_create(0);
    size_t err = 0;
    RJson *v = rjson_parse(a, resp, strlen(resp), &err);
    free(resp);
    int rc = -1;
    if (v) {
        const char *cid = json_str_field(v, "client_id");
        const char *csecret = json_str_field(v, "client_secret");
        if (cid) {
            free(o->client_id);
            o->client_id = strdup(cid);
            if (csecret) {
                free(o->client_secret);
                o->client_secret = strdup(csecret);
            }
            rc = 0;
        }
    }
    arena_destroy(a);
    return rc;
}

/* ---------- 3. PKCE ---------- */

static const char PKCE_CHARS[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";

int rk_mcp_oauth_pkce(char verifier[64], char challenge[64]) {
    if (!verifier || !challenge) return -1;
    uint8_t rand_bytes[43];
    if (RAND_bytes(rand_bytes, (int)sizeof(rand_bytes)) != 1) return -1;
    for (size_t i = 0; i < sizeof(rand_bytes); i++) {
        verifier[i] = PKCE_CHARS[rand_bytes[i] % (sizeof(PKCE_CHARS) - 1)];
    }
    verifier[43] = '\0';
    /* S256：SHA-256(verifier) → base64url 无 padding */
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)verifier, 43, digest);
    char *b64 = rk_oauth_b64url_encode(digest, SHA256_DIGEST_LENGTH);
    if (!b64) return -1;
    snprintf(challenge, 64, "%s", b64);
    free(b64);
    return 0;
}

/* ---------- 4. 授权 URL ---------- */

char *rk_mcp_oauth_authorize_url(RkMcpOAuth *o, const char *redirect_uri,
                                 const char *verifier, const char *state) {
    if (!o || !o->authorization_endpoint || !o->client_id || !redirect_uri || !verifier)
        return NULL;
    char challenge[64];
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char *)verifier, strlen(verifier), digest);
    char *ch = rk_oauth_b64url_encode(digest, SHA256_DIGEST_LENGTH);
    if (!ch) return NULL;
    snprintf(challenge, sizeof(challenge), "%s", ch);
    free(ch);
    char *er = rk_oauth_urlencode(redirect_uri);
    char *ecid = rk_oauth_urlencode(o->client_id);
    char *escope = o->scope ? rk_oauth_urlencode(o->scope) : NULL;
    char *estate = state ? rk_oauth_urlencode(state) : NULL;
    size_t cap = strlen(o->authorization_endpoint) + 512 +
                 (escope ? strlen(escope) : 0) + (estate ? strlen(estate) : 0);
    char *url = (char *)malloc(cap);
    int n;
    if (url) {
        n = snprintf(url, cap,
                     "%s?response_type=code&client_id=%s&redirect_uri=%s"
                     "&code_challenge=%s&code_challenge_method=S256%s%s%s%s",
                     o->authorization_endpoint, ecid ? ecid : "", er ? er : "",
                     challenge,
                     escope ? "&scope=" : "", escope ? escope : "",
                     estate ? "&state=" : "", estate ? estate : "");
        if (n <= 0 || (size_t)n >= cap) {
            free(url);
            url = NULL;
        }
    }
    free(er);
    free(ecid);
    free(escope);
    free(estate);
    return url;
}

int rk_mcp_oauth_parse_callback(const char *uri,
                                char *code, size_t code_sz,
                                char *state, size_t state_sz) {
    if (!uri) return -1;
    const char *q = strchr(uri, '?');
    if (!q) return -1;
    q++;
    const char *p = q;
    while (*p) {
        const char *amp = strchr(p, '&');
        size_t kvlen = amp ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', kvlen);
        if (eq) {
            size_t klen = (size_t)(eq - p);
            const char *val = eq + 1;
            size_t vlen = kvlen - klen - 1;
            if (klen == 4 && memcmp(p, "code", 4) == 0 && code && vlen < code_sz) {
                memcpy(code, val, vlen);
                code[vlen] = '\0';
            } else if (klen == 5 && memcmp(p, "state", 5) == 0 && state && vlen < state_sz) {
                memcpy(state, val, vlen);
                state[vlen] = '\0';
            }
        }
        if (!amp) break;
        p = amp + 1;
    }
    return (code && code[0]) ? 0 : -1;
}

/* ---------- 5. 令牌交换 / 刷新 ---------- */

static int token_request(RkMcpOAuth *o, const char *form_body, int timeout_ms) {
    if (!o || !o->token_endpoint) return -1;
    const char *hdrs[] = {
        "Content-Type", "application/x-www-form-urlencoded",
        "Accept", "application/json",
        NULL
    };
    int status = 0;
    char *resp = http_req(o->token_endpoint, "POST", hdrs, form_body, strlen(form_body),
                          timeout_ms, &status);
    if (!resp || status < 200 || status >= 300) {
        free(resp);
        return -1;
    }
    Arena *a = arena_create(0);
    size_t err = 0;
    RJson *v = rjson_parse(a, resp, strlen(resp), &err);
    free(resp);
    int rc = -1;
    if (v) {
        const char *at = json_str_field(v, "access_token");
        const char *rt = json_str_field(v, "refresh_token");
        const char *scope = json_str_field(v, "scope");
        const RJson *ei = rjson_obj_get(v, "expires_in");
        if (at) {
            free(o->access_token);
            o->access_token = strdup(at);
            if (rt) { free(o->refresh_token); o->refresh_token = strdup(rt); }
            if (scope) { free(o->scope); o->scope = strdup(scope); }
            if (ei && ei->type == RJSON_NUMBER) {
                o->expires_in = (int64_t)ei->u.number;
                o->expires_at = (int64_t)time(NULL) + o->expires_in;
            }
            rc = 0;
        }
    }
    arena_destroy(a);
    return rc;
}

int rk_mcp_oauth_exchange(RkMcpOAuth *o, const char *redirect_uri,
                          const char *code, const char *verifier, int timeout_ms) {
    if (!o || !code || !verifier) return -1;
    char *ecid = rk_oauth_urlencode(o->client_id ? o->client_id : "");
    char *ecode = rk_oauth_urlencode(code);
    char *eredir = rk_oauth_urlencode(redirect_uri ? redirect_uri : "");
    char *everifier = rk_oauth_urlencode(verifier);
    char body[4096];
    int n = snprintf(body, sizeof(body),
                     "grant_type=authorization_code&client_id=%s&code=%s"
                     "&redirect_uri=%s&code_verifier=%s",
                     ecid ? ecid : "", ecode ? ecode : "",
                     eredir ? eredir : "", everifier ? everifier : "");
    free(ecid);
    free(ecode);
    free(eredir);
    free(everifier);
    if (n < 0 || (size_t)n >= sizeof(body)) return -1;
    return token_request(o, body, timeout_ms);
}

int rk_mcp_oauth_refresh(RkMcpOAuth *o, int timeout_ms) {
    if (!o || !o->refresh_token) return -1;
    char *ecid = rk_oauth_urlencode(o->client_id ? o->client_id : "");
    char *ert = rk_oauth_urlencode(o->refresh_token);
    char body[4096];
    int n = snprintf(body, sizeof(body),
                     "grant_type=refresh_token&refresh_token=%s&client_id=%s",
                     ert ? ert : "", ecid ? ecid : "");
    free(ecid);
    free(ert);
    if (n < 0 || (size_t)n >= sizeof(body)) return -1;
    return token_request(o, body, timeout_ms);
}
