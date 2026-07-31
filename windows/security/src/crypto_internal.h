#ifndef RCSECURITY_CRYPTO_INTERNAL_H
#define RCSECURITY_CRYPTO_INTERNAL_H

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "rcsecurity/security.h"

namespace rcsecurity::detail {

Error randomBytes(uint8_t* out, size_t size);
Error sha256(const uint8_t* data, size_t size, Key32& out);
Error hmacSha256(const uint8_t* key, size_t keySize,
                 const uint8_t* data, size_t dataSize, Key32& out);
Error hkdfSha256(const uint8_t* key, size_t keySize,
                 const uint8_t* salt, size_t saltSize,
                 const uint8_t* info, size_t infoSize,
                 uint8_t* out, size_t outSize);
bool constantTimeEqual(const uint8_t* left, const uint8_t* right, size_t size);
void secureClear(void* data, size_t size);
void appendBigEndian64(Bytes& out, uint64_t value);
void appendLittleEndian64(Bytes& out, uint64_t value);
void appendLengthPrefixed(Bytes& out, const uint8_t* data, size_t size);
void appendLengthPrefixed(Bytes& out, std::string_view value);
bool validCanonicalId(std::string_view value);

}  // namespace rcsecurity::detail

#endif  // RCSECURITY_CRYPTO_INTERNAL_H
