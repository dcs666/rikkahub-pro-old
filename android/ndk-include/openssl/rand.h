/* OpenSSL rand shim（见 ssl.h 说明） */
#ifndef RIKKA_NDK_OPENSSL_RAND_H
#define RIKKA_NDK_OPENSSL_RAND_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 返回 1 成功 */
int RAND_bytes(unsigned char *buf, int num);

#ifdef __cplusplus
}
#endif

#endif /* RIKKA_NDK_OPENSSL_RAND_H */
