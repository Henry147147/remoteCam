#include "rcsecurity/security.h"

#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/macros.h>
#include <openssl/params.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <limits>
#include <map>
#include <mutex>
#include <utility>

#include "crypto_internal.h"

namespace rcsecurity {
namespace detail {
namespace {

template <typename T, void (*Free)(T*)>
struct OpenSslDeleter {
  void operator()(T* value) const {
    if (value != nullptr) Free(value);
  }
};

using Kdf = std::unique_ptr<EVP_KDF, OpenSslDeleter<EVP_KDF, EVP_KDF_free>>;
using KdfContext =
    std::unique_ptr<EVP_KDF_CTX, OpenSslDeleter<EVP_KDF_CTX, EVP_KDF_CTX_free>>;
using Mac = std::unique_ptr<EVP_MAC, OpenSslDeleter<EVP_MAC, EVP_MAC_free>>;
using MacContext =
    std::unique_ptr<EVP_MAC_CTX, OpenSslDeleter<EVP_MAC_CTX, EVP_MAC_CTX_free>>;

}  // namespace

Error randomBytes(uint8_t* out, size_t size) {
  if ((out == nullptr && size != 0) || size > static_cast<size_t>(INT_MAX)) {
    return Error::InvalidArgument;
  }
  return size == 0 || RAND_bytes_ex(nullptr, out, size, 256) == 1
             ? Error::None
             : Error::CryptoFailure;
}

Error sha256(const uint8_t* data, size_t size, Key32& out) {
  if (data == nullptr && size != 0) return Error::InvalidArgument;
  unsigned int written = 0;
  if (EVP_Digest(data, size, out.data(), &written, EVP_sha256(), nullptr) != 1 ||
      written != out.size()) {
    secureClear(out.data(), out.size());
    return Error::CryptoFailure;
  }
  return Error::None;
}

Error hmacSha256(const uint8_t* key, size_t keySize,
                 const uint8_t* data, size_t dataSize, Key32& out) {
  if ((key == nullptr && keySize != 0) || (data == nullptr && dataSize != 0)) {
    return Error::InvalidArgument;
  }
  Mac algorithm(EVP_MAC_fetch(nullptr, "HMAC", nullptr));
  if (!algorithm) return Error::CryptoFailure;
  MacContext context(EVP_MAC_CTX_new(algorithm.get()));
  if (!context) return Error::CryptoFailure;
  char digest[] = "SHA256";
  OSSL_PARAM parameters[] = {
      OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest, 0),
      OSSL_PARAM_construct_end()};
  size_t written = 0;
  if (EVP_MAC_init(context.get(), key, keySize, parameters) != 1 ||
      (dataSize != 0 && EVP_MAC_update(context.get(), data, dataSize) != 1) ||
      EVP_MAC_final(context.get(), out.data(), &written, out.size()) != 1 ||
      written != out.size()) {
    secureClear(out.data(), out.size());
    return Error::CryptoFailure;
  }
  return Error::None;
}

Error hkdfSha256(const uint8_t* key, size_t keySize,
                 const uint8_t* salt, size_t saltSize,
                 const uint8_t* info, size_t infoSize,
                 uint8_t* out, size_t outSize) {
  if ((key == nullptr && keySize != 0) || (salt == nullptr && saltSize != 0) ||
      (info == nullptr && infoSize != 0) || (out == nullptr && outSize != 0)) {
    return Error::InvalidArgument;
  }
  Kdf algorithm(EVP_KDF_fetch(nullptr, "HKDF", nullptr));
  if (!algorithm) return Error::CryptoFailure;
  KdfContext context(EVP_KDF_CTX_new(algorithm.get()));
  if (!context) return Error::CryptoFailure;

  char digest[] = "SHA256";
  char mode[] = "EXTRACT_AND_EXPAND";
  uint8_t empty = 0;
  OSSL_PARAM parameters[] = {
      OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_DIGEST, digest, 0),
      OSSL_PARAM_construct_utf8_string(OSSL_KDF_PARAM_MODE, mode, 0),
      OSSL_PARAM_construct_octet_string(
          OSSL_KDF_PARAM_KEY, const_cast<uint8_t*>(key), keySize),
      OSSL_PARAM_construct_octet_string(
          OSSL_KDF_PARAM_SALT,
          saltSize == 0 ? static_cast<void*>(&empty)
                        : const_cast<uint8_t*>(salt),
          saltSize),
      OSSL_PARAM_construct_octet_string(
          OSSL_KDF_PARAM_INFO,
          infoSize == 0 ? static_cast<void*>(&empty)
                        : const_cast<uint8_t*>(info),
          infoSize),
      OSSL_PARAM_construct_end()};
  if (EVP_KDF_derive(context.get(), out, outSize, parameters) != 1) {
    if (out != nullptr) secureClear(out, outSize);
    return Error::CryptoFailure;
  }
  return Error::None;
}

bool constantTimeEqual(const uint8_t* left, const uint8_t* right, size_t size) {
  if ((left == nullptr || right == nullptr) && size != 0) return false;
  return size == 0 || CRYPTO_memcmp(left, right, size) == 0;
}

void secureClear(void* data, size_t size) {
  if (data != nullptr && size != 0) OPENSSL_cleanse(data, size);
}

void appendBigEndian64(Bytes& out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>(value >> shift));
  }
}

void appendLittleEndian64(Bytes& out, uint64_t value) {
  for (int shift = 0; shift <= 56; shift += 8) {
    out.push_back(static_cast<uint8_t>(value >> shift));
  }
}

void appendLengthPrefixed(Bytes& out, const uint8_t* data, size_t size) {
  appendLittleEndian64(out, static_cast<uint64_t>(size));
  if (size != 0) out.insert(out.end(), data, data + size);
}

void appendLengthPrefixed(Bytes& out, std::string_view value) {
  appendLengthPrefixed(out, reinterpret_cast<const uint8_t*>(value.data()), value.size());
}

bool validCanonicalId(std::string_view value) {
  if (value.size() != 16) return false;
  return std::all_of(value.begin(), value.end(), [](char character) {
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f');
  });
}

}  // namespace detail

namespace {

constexpr uint8_t kEncryptedFlag = 1u << 1;
constexpr size_t kControlTagBytes = 32;
constexpr size_t kMediaTagBytes = 16;
constexpr size_t kSequenceBytes = 8;
constexpr size_t kMaximumWirePayload = 16u * 1024u * 1024u;
constexpr size_t kMaximumControlPlaintext = 1024u * 1024u;
constexpr std::string_view kControlDomain = "RemoteCam control envelope v1";
constexpr std::string_view kMediaDomain = "RemoteCam media envelope v1";

void appendBigEndian32(Bytes& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value >> 24));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}

uint64_t readBigEndian64(const uint8_t* data) {
  uint64_t value = 0;
  for (size_t index = 0; index < 8; ++index) value = (value << 8) | data[index];
  return value;
}

Bytes envelopeAuthenticationData(size_t payloadSize, uint8_t channel, uint8_t flags,
                                 uint64_t ptsMicros, uint64_t sequence,
                                 std::string_view domain) {
  Bytes aad;
  aad.reserve(16 + 8 + domain.size());
  appendBigEndian32(aad, static_cast<uint32_t>(payloadSize));
  aad.push_back(channel);
  aad.push_back(flags);
  aad.push_back(0);
  aad.push_back(0);
  detail::appendBigEndian64(aad, ptsMicros);
  detail::appendBigEndian64(aad, sequence);
  aad.insert(aad.end(), domain.begin(), domain.end());
  return aad;
}

std::array<uint8_t, 12> mediaNonce(const std::array<uint8_t, 4>& prefix,
                                   uint64_t sequence) {
  std::array<uint8_t, 12> nonce{};
  std::copy(prefix.begin(), prefix.end(), nonce.begin());
  for (size_t index = 0; index < 8; ++index) {
    nonce[4 + index] = static_cast<uint8_t>(sequence >> (56 - index * 8));
  }
  return nonce;
}

struct CipherDeleter {
  void operator()(EVP_CIPHER* value) const {
    if (value != nullptr) EVP_CIPHER_free(value);
  }
};
struct CipherContextDeleter {
  void operator()(EVP_CIPHER_CTX* value) const {
    if (value != nullptr) EVP_CIPHER_CTX_free(value);
  }
};
using Cipher = std::unique_ptr<EVP_CIPHER, CipherDeleter>;
using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, CipherContextDeleter>;

Error encryptChaCha(const Key32& key, const std::array<uint8_t, 12>& nonce,
                    const Bytes& aad, const uint8_t* plaintext, size_t plaintextSize,
                    Bytes& ciphertext, std::array<uint8_t, kMediaTagBytes>& tag) {
  if ((plaintext == nullptr && plaintextSize != 0) ||
      plaintextSize > static_cast<size_t>(INT_MAX) ||
      aad.size() > static_cast<size_t>(INT_MAX)) {
    return Error::InvalidArgument;
  }
  Cipher algorithm(EVP_CIPHER_fetch(nullptr, "ChaCha20-Poly1305", nullptr));
  CipherContext context(EVP_CIPHER_CTX_new());
  if (!algorithm || !context ||
      EVP_EncryptInit_ex2(context.get(), algorithm.get(), nullptr, nullptr, nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_IVLEN,
                          static_cast<int>(nonce.size()), nullptr) != 1 ||
      EVP_EncryptInit_ex2(context.get(), nullptr, key.data(), nonce.data(), nullptr) != 1) {
    return Error::CryptoFailure;
  }
  int written = 0;
  if (EVP_EncryptUpdate(context.get(), nullptr, &written, aad.data(),
                        static_cast<int>(aad.size())) != 1) {
    return Error::CryptoFailure;
  }
  ciphertext.resize(plaintextSize);
  if (plaintextSize != 0 &&
      EVP_EncryptUpdate(context.get(), ciphertext.data(), &written, plaintext,
                        static_cast<int>(plaintextSize)) != 1) {
    ciphertext.clear();
    return Error::CryptoFailure;
  }
  const int payloadWritten = plaintextSize == 0 ? 0 : written;
  std::array<uint8_t, 16> scratch{};
  int finalWritten = 0;
  if (EVP_EncryptFinal_ex(context.get(), scratch.data(), &finalWritten) != 1 ||
      payloadWritten != static_cast<int>(plaintextSize) || finalWritten != 0 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_GET_TAG,
                          static_cast<int>(tag.size()), tag.data()) != 1) {
    detail::secureClear(ciphertext.data(), ciphertext.size());
    ciphertext.clear();
    return Error::CryptoFailure;
  }
  return Error::None;
}

Error decryptChaCha(const Key32& key, const std::array<uint8_t, 12>& nonce,
                    const Bytes& aad, const uint8_t* ciphertext, size_t ciphertextSize,
                    const uint8_t* tag, Bytes& plaintext) {
  if ((ciphertext == nullptr && ciphertextSize != 0) || tag == nullptr ||
      ciphertextSize > static_cast<size_t>(INT_MAX) ||
      aad.size() > static_cast<size_t>(INT_MAX)) {
    return Error::InvalidArgument;
  }
  Cipher algorithm(EVP_CIPHER_fetch(nullptr, "ChaCha20-Poly1305", nullptr));
  CipherContext context(EVP_CIPHER_CTX_new());
  if (!algorithm || !context ||
      EVP_DecryptInit_ex2(context.get(), algorithm.get(), nullptr, nullptr, nullptr) != 1 ||
      EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_IVLEN,
                          static_cast<int>(nonce.size()), nullptr) != 1 ||
      EVP_DecryptInit_ex2(context.get(), nullptr, key.data(), nonce.data(), nullptr) != 1) {
    return Error::CryptoFailure;
  }
  int written = 0;
  if (EVP_DecryptUpdate(context.get(), nullptr, &written, aad.data(),
                        static_cast<int>(aad.size())) != 1) {
    return Error::CryptoFailure;
  }
  plaintext.resize(ciphertextSize);
  if (ciphertextSize != 0 &&
      EVP_DecryptUpdate(context.get(), plaintext.data(), &written, ciphertext,
                        static_cast<int>(ciphertextSize)) != 1) {
    plaintext.clear();
    return Error::CryptoFailure;
  }
  const int payloadWritten = ciphertextSize == 0 ? 0 : written;
  if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_AEAD_SET_TAG,
                          static_cast<int>(kMediaTagBytes),
                          const_cast<uint8_t*>(tag)) != 1) {
    plaintext.clear();
    return Error::CryptoFailure;
  }
  std::array<uint8_t, 16> scratch{};
  int finalWritten = 0;
  if (EVP_DecryptFinal_ex(context.get(), scratch.data(), &finalWritten) != 1 ||
      payloadWritten != static_cast<int>(ciphertextSize) || finalWritten != 0) {
    detail::secureClear(plaintext.data(), plaintext.size());
    plaintext.clear();
    return Error::AuthenticationFailed;
  }
  return Error::None;
}

struct SequenceCounter {
  uint64_t next = 0;
  bool exhausted = false;
};

class SessionProtector final : public ISessionProtector {
 public:
  SessionProtector(SessionRole role, const SessionKeys& keys, uint64_t expiresUnixSeconds)
      : outgoingControl_(role == SessionRole::Client ? keys.controlAtoB
                                                      : keys.controlBtoA),
        incomingControl_(role == SessionRole::Client ? keys.controlBtoA
                                                      : keys.controlAtoB),
        outgoingVideo_(role == SessionRole::Client ? keys.videoAtoB
                                                    : keys.videoBtoA),
        incomingVideo_(role == SessionRole::Client ? keys.videoBtoA
                                                    : keys.videoAtoB),
        outgoingStats_(role == SessionRole::Client ? keys.statsAtoB
                                                    : keys.statsBtoA),
        incomingStats_(role == SessionRole::Client ? keys.statsBtoA
                                                    : keys.statsAtoB),
        expiresUnixSeconds_(expiresUnixSeconds),
        steadyDeadline_(std::chrono::steady_clock::now() +
                        std::chrono::seconds(kMaximumSessionLifetimeSeconds)) {}

  ~SessionProtector() override {
    clearChannelKeys(outgoingControl_);
    clearChannelKeys(incomingControl_);
    clearChannelKeys(outgoingVideo_);
    clearChannelKeys(incomingVideo_);
    clearChannelKeys(outgoingStats_);
    clearChannelKeys(incomingStats_);
  }

  uint64_t expiresUnixSeconds() const override { return expiresUnixSeconds_; }

  Error protectControl(uint8_t channel, uint8_t flags, uint64_t ptsMicros,
                       const uint8_t* plaintext, size_t plaintextSize,
                       ProtectedPayload& out) override {
    if ((channel != 0 && channel != 3) || (flags & kEncryptedFlag) != 0 ||
        (plaintext == nullptr && plaintextSize != 0) ||
        plaintextSize > kMaximumControlPlaintext) {
      return Error::InvalidArgument;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (expired()) return Error::Expired;
    SequenceCounter& counter = outgoingControlSequence_[channel];
    if (counter.exhausted) return Error::SequenceExhausted;
    const uint64_t sequence = counter.next;
    const size_t envelopeSize = kSequenceBytes + plaintextSize + kControlTagBytes;
    Bytes authenticated = envelopeAuthenticationData(
        envelopeSize, channel, flags, ptsMicros, sequence, kControlDomain);
    if (plaintextSize != 0) {
      authenticated.insert(authenticated.end(), plaintext, plaintext + plaintextSize);
    }
    Key32 tag{};
    const ChannelKeys& channelKeys = outgoingAuthenticatedKeys(channel);
    const Error error = detail::hmacSha256(
        channelKeys.key.data(), channelKeys.key.size(), authenticated.data(),
        authenticated.size(), tag);
    detail::secureClear(authenticated.data(), authenticated.size());
    if (error != Error::None) return error;

    ProtectedPayload protectedPayload;
    protectedPayload.flags = flags;
    protectedPayload.payload.reserve(envelopeSize);
    detail::appendBigEndian64(protectedPayload.payload, sequence);
    if (plaintextSize != 0) {
      protectedPayload.payload.insert(protectedPayload.payload.end(), plaintext,
                                      plaintext + plaintextSize);
    }
    protectedPayload.payload.insert(protectedPayload.payload.end(), tag.begin(), tag.end());
    detail::secureClear(tag.data(), tag.size());
    advance(counter);
    out = std::move(protectedPayload);
    return Error::None;
  }

  Error unprotectControl(uint8_t channel, uint8_t flags, uint64_t ptsMicros,
                         const uint8_t* envelope, size_t envelopeSize,
                         Bytes& plaintext) override {
    if ((channel != 0 && channel != 3) || (flags & kEncryptedFlag) != 0 ||
        envelope == nullptr || envelopeSize < kSequenceBytes + kControlTagBytes ||
        envelopeSize > kMaximumControlPlaintext + kSequenceBytes + kControlTagBytes) {
      return Error::InvalidArgument;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (expired()) return Error::Expired;
    SequenceCounter& counter = incomingControlSequence_[channel];
    if (counter.exhausted) return Error::SequenceExhausted;
    const uint64_t sequence = readBigEndian64(envelope);
    if (sequence != counter.next) return Error::SequenceMismatch;
    const size_t plaintextSize = envelopeSize - kSequenceBytes - kControlTagBytes;
    Bytes authenticated = envelopeAuthenticationData(
        envelopeSize, channel, flags, ptsMicros, sequence, kControlDomain);
    authenticated.insert(authenticated.end(), envelope + kSequenceBytes,
                         envelope + kSequenceBytes + plaintextSize);
    Key32 expected{};
    const ChannelKeys& channelKeys = incomingAuthenticatedKeys(channel);
    const Error error = detail::hmacSha256(
        channelKeys.key.data(), channelKeys.key.size(), authenticated.data(),
        authenticated.size(), expected);
    detail::secureClear(authenticated.data(), authenticated.size());
    if (error != Error::None) return error;
    const bool matches = detail::constantTimeEqual(
        expected.data(), envelope + kSequenceBytes + plaintextSize, expected.size());
    detail::secureClear(expected.data(), expected.size());
    if (!matches) return Error::AuthenticationFailed;
    Bytes recovered(envelope + kSequenceBytes,
                    envelope + kSequenceBytes + plaintextSize);
    advance(counter);
    plaintext = std::move(recovered);
    return Error::None;
  }

  Error protectMedia(uint8_t channel, uint8_t flags, uint64_t ptsMicros,
                     const uint8_t* plaintext, size_t plaintextSize,
                     ProtectedPayload& out) override {
    if ((channel != 1 && channel != 3) || (flags & kEncryptedFlag) != 0 ||
        (plaintext == nullptr && plaintextSize != 0) ||
        plaintextSize > kMaximumWirePayload - kSequenceBytes - kMediaTagBytes) {
      return Error::InvalidArgument;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (expired()) return Error::Expired;
    SequenceCounter& counter = outgoingMediaSequence_[channel];
    if (counter.exhausted) return Error::SequenceExhausted;
    const uint64_t sequence = counter.next;
    const uint8_t protectedFlags = static_cast<uint8_t>(flags | kEncryptedFlag);
    const size_t envelopeSize = kSequenceBytes + plaintextSize + kMediaTagBytes;
    const Bytes aad = envelopeAuthenticationData(
        envelopeSize, channel, protectedFlags, ptsMicros, sequence, kMediaDomain);
    const ChannelKeys& channelKeys = outgoingMediaKeys(channel);
    const auto nonce = mediaNonce(channelKeys.noncePrefix, sequence);
    Bytes ciphertext;
    std::array<uint8_t, kMediaTagBytes> tag{};
    const Error error = encryptChaCha(channelKeys.key, nonce, aad, plaintext,
                                      plaintextSize, ciphertext, tag);
    if (error != Error::None) return error;
    ProtectedPayload protectedPayload;
    protectedPayload.flags = protectedFlags;
    protectedPayload.payload.reserve(envelopeSize);
    detail::appendBigEndian64(protectedPayload.payload, sequence);
    protectedPayload.payload.insert(protectedPayload.payload.end(), ciphertext.begin(),
                                    ciphertext.end());
    protectedPayload.payload.insert(protectedPayload.payload.end(), tag.begin(), tag.end());
    detail::secureClear(ciphertext.data(), ciphertext.size());
    detail::secureClear(tag.data(), tag.size());
    advance(counter);
    out = std::move(protectedPayload);
    return Error::None;
  }

  Error unprotectMedia(uint8_t channel, uint8_t flags, uint64_t ptsMicros,
                       const uint8_t* envelope, size_t envelopeSize,
                       Bytes& plaintext) override {
    if ((channel != 1 && channel != 3) || (flags & kEncryptedFlag) == 0 ||
        envelope == nullptr || envelopeSize < kSequenceBytes + kMediaTagBytes ||
        envelopeSize > kMaximumWirePayload) {
      return Error::InvalidArgument;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (expired()) return Error::Expired;
    SequenceCounter& counter = incomingMediaSequence_[channel];
    if (counter.exhausted) return Error::SequenceExhausted;
    const uint64_t sequence = readBigEndian64(envelope);
    if (sequence != counter.next) return Error::SequenceMismatch;
    const size_t ciphertextSize = envelopeSize - kSequenceBytes - kMediaTagBytes;
    const Bytes aad = envelopeAuthenticationData(
        envelopeSize, channel, flags, ptsMicros, sequence, kMediaDomain);
    const ChannelKeys& channelKeys = incomingMediaKeys(channel);
    const auto nonce = mediaNonce(channelKeys.noncePrefix, sequence);
    Bytes recovered;
    const Error error = decryptChaCha(
        channelKeys.key, nonce, aad, envelope + kSequenceBytes, ciphertextSize,
        envelope + kSequenceBytes + ciphertextSize, recovered);
    if (error != Error::None) return error;
    advance(counter);
    plaintext = std::move(recovered);
    return Error::None;
  }

 private:
  static void advance(SequenceCounter& counter) {
    if (counter.next + 1 >= kMaximumSessionRecordsPerChannel) {
      counter.exhausted = true;
    } else {
      ++counter.next;
    }
  }

  static void clearChannelKeys(ChannelKeys& keys) {
    detail::secureClear(keys.key.data(), keys.key.size());
    detail::secureClear(keys.noncePrefix.data(), keys.noncePrefix.size());
  }

  static uint64_t currentUnixSeconds() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(now).count();
    return seconds < 0 ? 0 : static_cast<uint64_t>(seconds);
  }

  bool expired() const {
    return currentUnixSeconds() >= expiresUnixSeconds_ ||
           std::chrono::steady_clock::now() >= steadyDeadline_;
  }

  const ChannelKeys& outgoingMediaKeys(uint8_t channel) const {
    return channel == 1 ? outgoingVideo_ : outgoingStats_;
  }

  const ChannelKeys& outgoingAuthenticatedKeys(uint8_t channel) const {
    return channel == 0 ? outgoingControl_ : outgoingStats_;
  }

  const ChannelKeys& incomingAuthenticatedKeys(uint8_t channel) const {
    return channel == 0 ? incomingControl_ : incomingStats_;
  }

  const ChannelKeys& incomingMediaKeys(uint8_t channel) const {
    return channel == 1 ? incomingVideo_ : incomingStats_;
  }

  ChannelKeys outgoingControl_{};
  ChannelKeys incomingControl_{};
  ChannelKeys outgoingVideo_{};
  ChannelKeys incomingVideo_{};
  ChannelKeys outgoingStats_{};
  ChannelKeys incomingStats_{};
  uint64_t expiresUnixSeconds_ = 0;
  std::chrono::steady_clock::time_point steadyDeadline_{};
  std::array<SequenceCounter, 256> outgoingControlSequence_{};
  std::array<SequenceCounter, 256> incomingControlSequence_{};
  std::array<SequenceCounter, 256> outgoingMediaSequence_{};
  std::array<SequenceCounter, 256> incomingMediaSequence_{};
  std::mutex mutex_;
};

Bytes authenticationInput(bool fromServer, std::string_view serviceId,
                          std::string_view deviceId, const Nonce32& serverNonce,
                          const Nonce32& clientNonce) {
  constexpr std::string_view clientDomain = "RemoteCam client authentication proof v1";
  constexpr std::string_view serverDomain = "RemoteCam server authentication proof v1";
  const std::string_view domain = fromServer ? serverDomain : clientDomain;
  Bytes input(domain.begin(), domain.end());
  detail::appendLengthPrefixed(input, deviceId);  // role A / phone
  detail::appendLengthPrefixed(input, serviceId); // role B / PC
  input.insert(input.end(), serverNonce.begin(), serverNonce.end());
  input.insert(input.end(), clientNonce.begin(), clientNonce.end());
  return input;
}

std::string pendingKey(std::string_view source, std::string_view deviceId) {
  std::string key(source);
  key.push_back('\0');
  key.append(deviceId);
  return key;
}

}  // namespace

bool available() { return true; }

Error makeAuthenticationProof(const Key32& pairingKey, bool fromServer,
                              std::string_view serviceId, std::string_view deviceId,
                              const Nonce32& serverNonce, const Nonce32& clientNonce,
                              Key32& proof) {
  if (!detail::validCanonicalId(serviceId) ||
      !detail::validCanonicalId(deviceId)) {
    return Error::InvalidArgument;
  }
  Bytes input = authenticationInput(fromServer, serviceId, deviceId,
                                    serverNonce, clientNonce);
  const Error error = detail::hmacSha256(pairingKey.data(), pairingKey.size(),
                                         input.data(), input.size(), proof);
  detail::secureClear(input.data(), input.size());
  return error;
}

Error deriveSessionKeys(const Key32& pairingKey, std::string_view serviceId,
                        std::string_view deviceId, const Nonce32& serverNonce,
                        const Nonce32& clientNonce, SessionKeys& keys) {
  if (!detail::validCanonicalId(serviceId) ||
      !detail::validCanonicalId(deviceId)) {
    return Error::InvalidArgument;
  }
  Bytes salt(serverNonce.begin(), serverNonce.end());
  salt.insert(salt.end(), clientNonce.begin(), clientNonce.end());
  constexpr std::string_view domain = "RemoteCam session keys v1";
  Bytes info(domain.begin(), domain.end());
  detail::appendLengthPrefixed(info, deviceId);  // role A / phone
  detail::appendLengthPrefixed(info, serviceId); // role B / PC
  constexpr size_t kChannelMaterialBytes = 32 + 4;
  std::array<uint8_t, 6 * kChannelMaterialBytes> material{};
  const Error error = detail::hkdfSha256(
      pairingKey.data(), pairingKey.size(), salt.data(), salt.size(), info.data(),
      info.size(), material.data(), material.size());
  detail::secureClear(salt.data(), salt.size());
  detail::secureClear(info.data(), info.size());
  if (error != Error::None) return error;
  size_t offset = 0;
  auto take = [&](ChannelKeys& channelKeys) {
    std::copy_n(material.data() + offset, channelKeys.key.size(),
                channelKeys.key.begin());
    offset += channelKeys.key.size();
    std::copy_n(material.data() + offset, channelKeys.noncePrefix.size(),
                channelKeys.noncePrefix.begin());
    offset += channelKeys.noncePrefix.size();
  };
  take(keys.controlAtoB);
  take(keys.controlBtoA);
  take(keys.videoAtoB);
  take(keys.videoBtoA);
  take(keys.statsAtoB);
  take(keys.statsBtoA);
  detail::secureClear(material.data(), material.size());
  return Error::None;
}

Error createSessionProtector(SessionRole role, const SessionKeys& keys,
                             uint64_t nowUnixSeconds,
                             std::shared_ptr<ISessionProtector>& protector) {
  if (nowUnixSeconds > std::numeric_limits<uint64_t>::max() -
                           kMaximumSessionLifetimeSeconds) {
    protector.reset();
    return Error::InvalidArgument;
  }
  try {
    protector = std::make_shared<SessionProtector>(
        role, keys, nowUnixSeconds + kMaximumSessionLifetimeSeconds);
  } catch (...) {
    protector.reset();
    return Error::CryptoFailure;
  }
  return Error::None;
}

struct StoredSessionSecurity::Impl {
  struct Pending {
    PairingRecord record;
    AuthenticationChallenge challenge;
    AttemptPolicy::AttemptId attemptId = 0;
  };

  Impl(PairingStore& pairingStore, std::string id, AttemptPolicyConfig config)
      : store(pairingStore), serviceId(std::move(id)), policy(std::move(config)) {}

  ~Impl() {
    for (auto& [key, pendingAttempt] : pending) {
      (void)key;
      detail::secureClear(pendingAttempt.record.key.data(), pendingAttempt.record.key.size());
    }
  }

  PairingStore& store;
  std::string serviceId;
  AttemptPolicy policy;
  std::map<std::string, Pending> pending;
  std::mutex mutex;
};

StoredSessionSecurity::StoredSessionSecurity(PairingStore& store, std::string serviceId,
                                             AttemptPolicyConfig policy)
    : impl_(std::make_unique<Impl>(store, std::move(serviceId), std::move(policy))) {}

StoredSessionSecurity::~StoredSessionSecurity() = default;

Error StoredSessionSecurity::beginAuthentication(
    std::string_view source, std::string_view deviceId, uint64_t nowUnixSeconds,
    AuthenticationChallenge& challenge) {
  if (!impl_ || source.empty() || source.size() > 256 ||
      !detail::validCanonicalId(impl_->serviceId) ||
      !detail::validCanonicalId(deviceId) ||
      nowUnixSeconds > std::numeric_limits<uint64_t>::max() - 10) {
    return Error::InvalidArgument;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  AttemptPolicy::AttemptId attemptId = 0;
  const Error policyError = impl_->policy.beginAttempt(
      std::string(source), AttemptPolicy::TimePoint::clock::now(), attemptId);
  if (policyError != Error::None) return policyError;

  PairingRecord record;
  const Error loadError = impl_->store.load(deviceId, record);
  if (loadError != Error::None) return loadError;
  if (record.expiresUnixSeconds != std::numeric_limits<uint64_t>::max() &&
      record.expiresUnixSeconds <= nowUnixSeconds) {
    detail::secureClear(record.key.data(), record.key.size());
    return Error::Expired;
  }

  AuthenticationChallenge generated;
  const Error randomError = detail::randomBytes(generated.serverNonce.data(),
                                                 generated.serverNonce.size());
  if (randomError != Error::None) {
    detail::secureClear(record.key.data(), record.key.size());
    return randomError;
  }
  generated.expiresUnixSeconds = nowUnixSeconds + 10;
  const std::string key = pendingKey(source, deviceId);
  const auto existing = impl_->pending.find(key);
  if (existing != impl_->pending.end()) {
    detail::secureClear(existing->second.record.key.data(),
                        existing->second.record.key.size());
    impl_->pending.erase(existing); // its provisional attempt deliberately remains charged
  }
  impl_->pending.emplace(key, Impl::Pending{record, generated, attemptId});
  detail::secureClear(record.key.data(), record.key.size());
  challenge = generated;
  return Error::None;
}

Error StoredSessionSecurity::finishAuthentication(
    std::string_view source, std::string_view deviceId,
    const Nonce32& clientNonce, const Key32& clientProof,
    uint64_t nowUnixSeconds, AuthenticationResult& result) {
  if (!impl_ || source.empty() || source.size() > 256 ||
      !detail::validCanonicalId(deviceId)) {
    return Error::InvalidArgument;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  const std::string key = pendingKey(source, deviceId);
  auto it = impl_->pending.find(key);
  if (it == impl_->pending.end()) {
    AttemptPolicy::AttemptId unsolicited = 0;
    const Error policyError = impl_->policy.beginAttempt(
        std::string(source), AttemptPolicy::TimePoint::clock::now(), unsolicited);
    if (policyError != Error::None) return policyError;
    impl_->policy.finishAttempt(unsolicited, false, AttemptPolicy::TimePoint::clock::now());
    return Error::AuthenticationFailed;
  }
  Impl::Pending pending = std::move(it->second);
  impl_->pending.erase(it);
  const auto finishPolicy = [&](bool success) {
    impl_->policy.finishAttempt(pending.attemptId, success,
                                AttemptPolicy::TimePoint::clock::now());
  };
  if (nowUnixSeconds >= pending.challenge.expiresUnixSeconds ||
      (pending.record.expiresUnixSeconds != std::numeric_limits<uint64_t>::max() &&
       nowUnixSeconds >= pending.record.expiresUnixSeconds)) {
    finishPolicy(false);
    detail::secureClear(pending.record.key.data(), pending.record.key.size());
    return Error::Expired;
  }

  Key32 expectedClientProof{};
  Error error = makeAuthenticationProof(
      pending.record.key, false, impl_->serviceId, deviceId,
      pending.challenge.serverNonce, clientNonce, expectedClientProof);
  if (error != Error::None || !detail::constantTimeEqual(
          expectedClientProof.data(), clientProof.data(), clientProof.size())) {
    finishPolicy(false);
    detail::secureClear(expectedClientProof.data(), expectedClientProof.size());
    detail::secureClear(pending.record.key.data(), pending.record.key.size());
    return error == Error::None ? Error::AuthenticationFailed : error;
  }
  detail::secureClear(expectedClientProof.data(), expectedClientProof.size());

  AuthenticationResult authenticated;
  error = makeAuthenticationProof(
      pending.record.key, true, impl_->serviceId, deviceId,
      pending.challenge.serverNonce, clientNonce, authenticated.serverProof);
  SessionKeys keys{};
  if (error == Error::None) {
    error = deriveSessionKeys(pending.record.key, impl_->serviceId, deviceId,
                              pending.challenge.serverNonce, clientNonce, keys);
  }
  if (error == Error::None) {
    error = createSessionProtector(SessionRole::Server, keys, nowUnixSeconds,
                                   authenticated.protector);
  }
  const auto clear = [](ChannelKeys& channelKeys) {
    detail::secureClear(channelKeys.key.data(), channelKeys.key.size());
    detail::secureClear(channelKeys.noncePrefix.data(), channelKeys.noncePrefix.size());
  };
  clear(keys.controlAtoB);
  clear(keys.controlBtoA);
  clear(keys.videoAtoB);
  clear(keys.videoBtoA);
  clear(keys.statsAtoB);
  clear(keys.statsBtoA);
  detail::secureClear(pending.record.key.data(), pending.record.key.size());
  if (error != Error::None) {
    finishPolicy(false);
    return error;
  }
  finishPolicy(true);
  result = std::move(authenticated);
  return Error::None;
}

}  // namespace rcsecurity
