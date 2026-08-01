/* OpenSSL sha shim（见 ssl.h 说明） */
#ifndef RIKKA_NDK_OPENSSL_SHA_H
#define RIKKA_NDK_OPENSSL_SHA_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SHA256_DIGEST_LENGTH 32

typedef struct SHA256state_st {
    unsigned int h[8];
    unsigned long long Nl, Nh;
    unsigned char data[128];
    unsigned int num;
} SHA256_CTX;

int SHA256_Init(SHA256_CTX *c);
int SHA256_Update(SHA256_CTX *c, const void *data, size_t len);
int SHA256_Final(unsigned char *md, SHA256_CTX *c);

/* 便捷单次调用 */
unsigned char *SHA256(const unsigned char *d, size_t n, unsigned char *md);

#ifdef __cplusplus
}
#endif

#endif /* RIKKA_NDK_OPENSSL_SHA_H */
