#include <cstddef>
#include <cstdint>
extern "C" {
#include "libsha1/libsha1.h"
void esp_sha(int, const unsigned char* input, size_t length, unsigned char* output) {
  SHA1_CTX context;
  SHA1Init(&context);
  SHA1Update(&context, input, static_cast<uint32_t>(length));
  SHA1Final(output, &context);
}
}
