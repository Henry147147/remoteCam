// RemoteCam production session security.

#ifndef RCSECURITY_SECURITY_H
#define RCSECURITY_SECURITY_H

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rcsecurity {

using Bytes = std::vector<uint8_t>;
using Key16 = std::array<uint8_t, 16>;
using Key32 = std::array<uint8_t, 32>;
using Nonce32 = std::array<uint8_t, 32>;
using Point65 = std::array<uint8_t, 65>;
using Salt16 = std::array<uint8_t, 16>;

enum class Error {
  None = 0,
  Unavailable,
  InvalidArgument,
  CryptoFailure,
  InvalidPoint,
  AuthenticationFailed,
  SequenceMismatch,
  SequenceExhausted,
  Expired,
  RateLimited,
  NotFound,
  StorageFailure,
  CorruptRecord,
};

const char* errorText(Error error);
bool available();

// RFC 9382 SPAKE2-P256-SHA256-HKDF-HMAC. The phone/client is role A and uses M; the
// PC/server is role B and uses N. Points use the uncompressed 65-byte SEC1 form.
enum class SpakeRole { A, B };

struct Spake2Secrets {
  Bytes transcript;
  Point65 sharedPoint{};
  Key16 sharedSecret{};       // RFC 9382 Ke; feed to the application HKDF only.
  Key16 confirmationKeyA{};
  Key16 confirmationKeyB{};
  Key32 confirmationA{};
  Key32 confirmationB{};
};

class Spake2P256 final {
 public:
  Spake2P256();
  ~Spake2P256();
  Spake2P256(Spake2P256&&) noexcept;
  Spake2P256& operator=(Spake2P256&&) noexcept;
  Spake2P256(const Spake2P256&) = delete;
  Spake2P256& operator=(const Spake2P256&) = delete;

  static Error create(SpakeRole role, std::string identityA, std::string identityB,
                      Bytes aad, const Key32& passwordScalar, Spake2P256& out);
  // Deterministic scalar injection exists only for RFC test vectors. Production code
  // must call create(), which samples a fresh non-zero scalar from OpenSSL's CSPRNG.
  static Error createDeterministic(SpakeRole role, std::string identityA,
                                   std::string identityB, Bytes aad,
                                   const Key32& passwordScalar,
                                   const Key32& privateScalar, Spake2P256& out);

  const Point65& publicValue() const;
  Error finish(const Point65& peerPublicValue, Spake2Secrets& out);

 private:
  struct Impl;
  explicit Spake2P256(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

// Six ASCII digits (leading zeroes included) are stretched to 320 bits using scrypt
// N=32768,r=8,p=1,maxmem=64 MiB, then reduced modulo the P-256 group order.
Error derivePasswordScalar(std::string_view sixDigitCode, const Salt16& salt,
                           Key32& scalar);

struct PairingOffer {
  Salt16 salt{};
  Point65 publicValueB{};
  uint64_t expiresUnixSeconds = 0;
};

struct PairingRecord {
  std::string serviceId;
  std::string deviceId;
  Key32 key{};
  uint64_t createdUnixSeconds = 0;
  // UINT64_MAX is the v1 non-expiring sentinel. Long-term pairing keys live until
  // the user explicitly unpairs; only PAKE/authentication/session state has a TTL.
  uint64_t expiresUnixSeconds = 0;
};

class PairingServer final {
 public:
  PairingServer();
  ~PairingServer();
  PairingServer(PairingServer&&) noexcept;
  PairingServer& operator=(PairingServer&&) noexcept;
  PairingServer(const PairingServer&) = delete;
  PairingServer& operator=(const PairingServer&) = delete;

  static Error begin(std::string serviceId, std::string deviceId,
                     std::string_view sixDigitCode, uint64_t nowUnixSeconds,
                     PairingServer& out);
  const PairingOffer& offer() const;
  Error receiveCommit(const Point65& publicValueA, Key32& confirmationB);
  Error verifyPeer(const Key32& confirmationA, uint64_t nowUnixSeconds,
                   PairingRecord& record);

 private:
  struct Impl;
  explicit PairingServer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

// DPAPI CurrentUser protects the complete record. Writes use a same-directory temp
// file, FlushFileBuffers, and MoveFileEx(REPLACE_EXISTING|WRITE_THROUGH).
class PairingStore final {
 public:
  PairingStore(std::filesystem::path directory, std::string serviceId);

  static std::filesystem::path defaultDirectory();
  Error save(const PairingRecord& record) const;
  Error load(std::string_view deviceId, PairingRecord& record) const;
  Error erase(std::string_view deviceId) const;

 private:
  std::filesystem::path directory_;
  std::string serviceId_;
};

struct AttemptPolicyConfig {
  size_t maxAttemptsPerSource = 5;
  size_t maxAttemptsGlobal = 5;
  std::chrono::seconds failureWindow{600};
  std::chrono::seconds cooldown{300};
  std::chrono::seconds pairingLifetime{120};
  std::chrono::seconds authenticationLifetime{10};
};

class AttemptPolicy final {
 public:
  using TimePoint = std::chrono::steady_clock::time_point;
  using AttemptId = uint64_t;
  explicit AttemptPolicy(AttemptPolicyConfig config = {});

  bool allowed(std::string_view peer, TimePoint now) const;
  // Starting charges a provisional failure to both the source and global budgets.
  // A successful completion removes exactly that charge; failure or abandonment
  // leaves it in place for the rate window.
  Error beginAttempt(std::string peer, TimePoint now, AttemptId& id);
  void finishAttempt(AttemptId id, bool succeeded, TimePoint now);
  bool pairingExpired(TimePoint started, TimePoint now) const;
  bool authenticationExpired(TimePoint started, TimePoint now) const;

 private:
  struct Attempt {
    AttemptId id = 0;
    std::string peer;
    TimePoint started{};
  };
  void prune(TimePoint now) const;
  size_t peerAttempts(std::string_view peer) const;

  AttemptPolicyConfig config_;
  mutable std::vector<Attempt> attempts_;
  mutable std::unordered_map<std::string, TimePoint> peerBlockedUntil_;
  mutable std::optional<TimePoint> globalBlockedUntil_;
  AttemptId nextAttemptId_ = 1;
};

struct ChannelKeys {
  Key32 key{};
  std::array<uint8_t, 4> noncePrefix{};
};

struct SessionKeys {
  ChannelKeys controlAtoB{};
  ChannelKeys controlBtoA{};
  ChannelKeys videoAtoB{};
  ChannelKeys videoBtoA{};
  ChannelKeys statsAtoB{};
  ChannelKeys statsBtoA{};
};

inline constexpr uint64_t kMaximumSessionRecordsPerChannel = uint64_t{1} << 32;
inline constexpr uint64_t kMaximumSessionLifetimeSeconds = 24ull * 60ull * 60ull;

Error makeAuthenticationProof(const Key32& pairingKey, bool fromServer,
                              std::string_view serviceId, std::string_view deviceId,
                              const Nonce32& serverNonce, const Nonce32& clientNonce,
                              Key32& proof);
Error deriveSessionKeys(const Key32& pairingKey, std::string_view serviceId,
                        std::string_view deviceId, const Nonce32& serverNonce,
                        const Nonce32& clientNonce, SessionKeys& keys);

struct ProtectedPayload {
  uint8_t flags = 0;
  Bytes payload;
};

class ISessionProtector {
 public:
  virtual ~ISessionProtector() = default;
  virtual uint64_t expiresUnixSeconds() const = 0;
  virtual Error protectControl(uint8_t channel, uint8_t flags, uint64_t ptsMicros,
                               const uint8_t* plaintext, size_t plaintextSize,
                               ProtectedPayload& out) = 0;
  virtual Error unprotectControl(uint8_t channel, uint8_t flags, uint64_t ptsMicros,
                                 const uint8_t* envelope, size_t envelopeSize,
                                 Bytes& plaintext) = 0;
  virtual Error protectMedia(uint8_t channel, uint8_t flags, uint64_t ptsMicros,
                             const uint8_t* plaintext, size_t plaintextSize,
                             ProtectedPayload& out) = 0;
  virtual Error unprotectMedia(uint8_t channel, uint8_t flags, uint64_t ptsMicros,
                               const uint8_t* envelope, size_t envelopeSize,
                               Bytes& plaintext) = 0;
};

enum class SessionRole { Server, Client };
Error createSessionProtector(SessionRole role, const SessionKeys& keys,
                             uint64_t nowUnixSeconds,
                             std::shared_ptr<ISessionProtector>& protector);

struct AuthenticationChallenge {
  Nonce32 serverNonce{};
  uint64_t expiresUnixSeconds = 0;
};

struct AuthenticationResult {
  Key32 serverProof{};
  std::shared_ptr<ISessionProtector> protector;
};

// A claimed device ID only selects a DPAPI record. It never grants trust. The caller
// must complete the nonce-bound HMAC proof before it receives a session protector.
class ISessionSecurity {
 public:
  virtual ~ISessionSecurity() = default;
  virtual Error beginAuthentication(std::string_view source,
                                    std::string_view deviceId,
                                    uint64_t nowUnixSeconds,
                                    AuthenticationChallenge& challenge) = 0;
  virtual Error finishAuthentication(std::string_view source,
                                     std::string_view deviceId,
                                     const Nonce32& clientNonce,
                                     const Key32& clientProof,
                                     uint64_t nowUnixSeconds,
                                     AuthenticationResult& result) = 0;
};

class StoredSessionSecurity final : public ISessionSecurity {
 public:
  StoredSessionSecurity(PairingStore& store, std::string serviceId,
                        AttemptPolicyConfig policy = {});
  ~StoredSessionSecurity() override;

  Error beginAuthentication(std::string_view source, std::string_view deviceId,
                            uint64_t nowUnixSeconds,
                            AuthenticationChallenge& challenge) override;
  Error finishAuthentication(std::string_view source, std::string_view deviceId,
                             const Nonce32& clientNonce,
                             const Key32& clientProof,
                             uint64_t nowUnixSeconds,
                             AuthenticationResult& result) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rcsecurity

#endif  // RCSECURITY_SECURITY_H
