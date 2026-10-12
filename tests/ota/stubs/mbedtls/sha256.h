#pragma once
#include <CommonCrypto/CommonDigest.h>
using mbedtls_sha256_context = CC_SHA256_CTX;
inline void mbedtls_sha256_init(mbedtls_sha256_context*) {}
inline void mbedtls_sha256_free(mbedtls_sha256_context*) {}
inline int mbedtls_sha256_starts_ret(mbedtls_sha256_context* c,int) { return CC_SHA256_Init(c) ? 0 : -1; }
inline int mbedtls_sha256_update_ret(mbedtls_sha256_context* c,const unsigned char* p,size_t n) { return CC_SHA256_Update(c,p,n) ? 0 : -1; }
inline int mbedtls_sha256_finish_ret(mbedtls_sha256_context* c,unsigned char* out) { return CC_SHA256_Final(out,c) ? 0 : -1; }
