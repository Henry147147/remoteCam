#include "rcsecurity/security.h"

#include <utility>

namespace rcsecurity {

bool available() { return false; }

struct Spake2P256::Impl {};

Spake2P256::Spake2P256() = default;
Spake2P256::~Spake2P256() = default;
Spake2P256::Spake2P256(Spake2P256&&) noexcept = default;
Spake2P256& Spake2P256::operator=(Spake2P256&&) noexcept = default;
Spake2P256::Spake2P256(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Error Spake2P256::create(SpakeRole, std::string, std::string, Bytes,
                         const Key32&, Spake2P256&) {
  return Error::Unavailable;
}

Error Spake2P256::createDeterministic(SpakeRole, std::string, std::string, Bytes,
                                      const Key32&, const Key32&, Spake2P256&) {
  return Error::Unavailable;
}

const Point65& Spake2P256::publicValue() const {
  static const Point65 empty{};
  return empty;
}

Error Spake2P256::finish(const Point65&, Spake2Secrets&) {
  return Error::Unavailable;
}

Error derivePasswordScalar(std::string_view, const Salt16&, Key32&) {
  return Error::Unavailable;
}

struct PairingServer::Impl {};

PairingServer::PairingServer() = default;
PairingServer::~PairingServer() = default;
PairingServer::PairingServer(PairingServer&&) noexcept = default;
PairingServer& PairingServer::operator=(PairingServer&&) noexcept = default;
PairingServer::PairingServer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Error PairingServer::begin(std::string, std::string, std::string_view, uint64_t,
                           PairingServer&) {
  return Error::Unavailable;
}

const PairingOffer& PairingServer::offer() const {
  static const PairingOffer empty{};
  return empty;
}

Error PairingServer::receiveCommit(const Point65&, Key32&) {
  return Error::Unavailable;
}

Error PairingServer::verifyPeer(const Key32&, uint64_t, PairingRecord&) {
  return Error::Unavailable;
}

Error makeAuthenticationProof(const Key32&, bool, std::string_view,
                              std::string_view, const Nonce32&, const Nonce32&,
                              Key32&) {
  return Error::Unavailable;
}

Error deriveSessionKeys(const Key32&, std::string_view, std::string_view,
                        const Nonce32&, const Nonce32&, SessionKeys&) {
  return Error::Unavailable;
}

Error createSessionProtector(SessionRole, const SessionKeys&,
                             uint64_t,
                             std::shared_ptr<ISessionProtector>& protector) {
  protector.reset();
  return Error::Unavailable;
}

struct StoredSessionSecurity::Impl {};

StoredSessionSecurity::StoredSessionSecurity(PairingStore&, std::string,
                                             AttemptPolicyConfig)
    : impl_(std::make_unique<Impl>()) {}
StoredSessionSecurity::~StoredSessionSecurity() = default;

Error StoredSessionSecurity::beginAuthentication(std::string_view, std::string_view,
                                                 uint64_t,
                                                 AuthenticationChallenge&) {
  return Error::Unavailable;
}

Error StoredSessionSecurity::finishAuthentication(
    std::string_view, std::string_view, const Nonce32&, const Key32&, uint64_t,
    AuthenticationResult&) {
  return Error::Unavailable;
}

}  // namespace rcsecurity
