/*
 * OpenSSL shim（仅 Android）：NDK 不提供 openssl 头文件，
 * 但系统 libssl.so（API 26+）提供全部符号。这里声明引擎用到的
 * 最小 API 子集（OpenSSL 1.1/3.x 兼容）。
 */
#ifndef RIKKA_NDK_OPENSSL_SSL_H
#define RIKKA_NDK_OPENSSL_SSL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_method_st SSL_METHOD;
typedef struct x509_store_st X509_STORE;
typedef struct x509_st X509;
typedef struct ssl_session_st SSL_SESSION;

#define SSL_VERIFY_PEER 1
#define SSL_VERIFY_NONE 0
#define TLS1_2_VERSION 0x0303

enum {
    SSL_ERROR_NONE = 0,
    SSL_ERROR_SSL = 1,
    SSL_ERROR_WANT_READ = 2,
    SSL_ERROR_WANT_WRITE = 3,
    SSL_ERROR_WANT_X509_LOOKUP = 4,
    SSL_ERROR_SYSCALL = 5,
    SSL_ERROR_ZERO_RETURN = 6,
    SSL_ERROR_WANT_CONNECT = 7,
    SSL_ERROR_WANT_ACCEPT = 8,
};

/* 1.1+ 为 no-op，声明为宏避免符号问题 */
#define SSL_library_init() 1
#define SSL_load_error_strings() ((void)0)

const SSL_METHOD *TLS_client_method(void);
SSL_CTX *SSL_CTX_new(const SSL_METHOD *meth);
void SSL_CTX_free(SSL_CTX *ctx);
void SSL_CTX_set_default_verify_paths(SSL_CTX *ctx);
int SSL_CTX_load_verify_locations(SSL_CTX *ctx, const char *cafile, const char *capath);
void SSL_CTX_set_verify(SSL_CTX *ctx, int mode, void *callback);
/* 3.x 中 set_min/max_proto_version 是宏 → SSL_CTX_ctrl, 不能声明为函数
   (libssl.so 不导出该符号, 会 dlopen 失败) */
long SSL_CTX_ctrl(SSL_CTX *ctx, int cmd, long larg, void *parg);
#define SSL_CTRL_SET_MIN_PROTO_VERSION 123
#define SSL_CTRL_SET_MAX_PROTO_VERSION 124
#define SSL_CTX_set_min_proto_version(ctx, version) \
    SSL_CTX_ctrl((ctx), SSL_CTRL_SET_MIN_PROTO_VERSION, (long)(version), NULL)
#define SSL_CTX_set_max_proto_version(ctx, version) \
    SSL_CTX_ctrl((ctx), SSL_CTRL_SET_MAX_PROTO_VERSION, (long)(version), NULL)

SSL *SSL_new(SSL_CTX *ctx);
void SSL_free(SSL *ssl);
int SSL_set_fd(SSL *ssl, int fd);
void SSL_set_connect_state(SSL *ssl);
int SSL_connect(SSL *ssl);
int SSL_read(SSL *ssl, void *buf, int num);
int SSL_write(SSL *ssl, const void *buf, int num);
int SSL_get_error(const SSL *ssl, int ret);
void SSL_shutdown(SSL *ssl);
long SSL_get_verify_result(const SSL *ssl);
#define X509_V_OK 0
#define X509_V_FLAG_PARTIAL_CHAIN 0x80000
int SSL_set1_host(SSL *ssl, const char *hostname);
X509_STORE *SSL_CTX_get_cert_store(const SSL_CTX *ctx);
void X509_STORE_set_flags(X509_STORE *ctx, long flags);
const char *X509_verify_cert_error_string(long n);
int X509_STORE_add_cert(X509_STORE *ctx, X509 *x);
X509 *d2i_X509(X509 **a, const unsigned char **pp, long length);
void X509_free(X509 *a);
SSL_CTX *SSL_get_SSL_CTX(const SSL *ssl);

/* SSL_set_tlsext_host_name 是宏（OpenSSL 1.1）；55 = SSL_CTRL_SET_TLSEXT_HOSTNAME */
long SSL_ctrl(SSL *ssl, int cmd, long larg, void *parg);
#define SSL_set_tlsext_host_name(s, name) \
    SSL_ctrl((s), 55, 0, (void *)(name))

#ifdef __cplusplus
}
#endif

#endif /* RIKKA_NDK_OPENSSL_SSL_H */
