/* OpenSSL err shim（见 ssl.h 说明） */
#ifndef RIKKA_NDK_OPENSSL_ERR_H
#define RIKKA_NDK_OPENSSL_ERR_H

#ifdef __cplusplus
extern "C" {
#endif

unsigned long ERR_get_error(void);
const char *ERR_reason_error_string(unsigned long e);

#ifdef __cplusplus
}
#endif

#endif /* RIKKA_NDK_OPENSSL_ERR_H */
