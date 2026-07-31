#include "rcsecurity/security.h"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <utility>

#include "crypto_internal.h"

namespace rcsecurity {
namespace {

constexpr std::array<uint8_t, 33> kPointM = {
    0x02, 0x88, 0x6e, 0x2f, 0x97, 0xac, 0xe4, 0x6e, 0x55, 0xba, 0x9d,
    0xd7, 0x24, 0x25, 0x79, 0xf2, 0x99, 0x3b, 0x64, 0xe1, 0x6e, 0xf3,
    0xdc, 0xab, 0x95, 0xaf, 0xd4, 0x97, 0x33, 0x3d, 0x8f, 0xa1, 0x2f};
constexpr std::array<uint8_t, 33> kPointN = {
    0x03, 0xd8, 0xbb, 0xd6, 0xc6, 0x39, 0xc6, 0x29, 0x37, 0xb0, 0x4d,
    0x99, 0x7f, 0x38, 0xc3, 0x77, 0x07, 0x19, 0xc6, 0x29, 0xd7, 0x01,
    0x4d, 0x49, 0xa2, 0x4b, 0x4f, 0x98, 0xba, 0xa1, 0x29, 0x2b, 0x49};
constexpr uint64_t kPairingLifetimeSeconds = 120;
constexpr std::string_view kPairingDomain = "RemoteCam SPAKE2 pairing v1";
constexpr std::string_view kRecordDomain = "RemoteCam long-term pairing key v1";

template <typename T, void (*Free)(T*)>
struct OpenSslDeleter {
  void operator()(T* value) const {
    if (value != nullptr) Free(value);
  }
};

using Group = std::unique_ptr<EC_GROUP, OpenSslDeleter<EC_GROUP, EC_GROUP_free>>;
using Point = std::unique_ptr<EC_POINT, OpenSslDeleter<EC_POINT, EC_POINT_free>>;
using BigNumber = std::unique_ptr<BIGNUM, OpenSslDeleter<BIGNUM, BN_clear_free>>;
using BigNumberContext =
    std::unique_ptr<BN_CTX, OpenSslDeleter<BN_CTX, BN_CTX_free>>;
using Kdf = std::unique_ptr<EVP_KDF, OpenSslDeleter<EVP_KDF, EVP_KDF_free>>;
using KdfContext =
    std::unique_ptr<EVP_KDF_CTX, OpenSslDeleter<EVP_KDF_CTX, EVP_KDF_CTX_free>>;

Error loadMaskPoint(const EC_GROUP* group, SpakeRole role, EC_POINT* point,
                    BN_CTX* context) {
  const auto& encoded = role == SpakeRole::A ? kPointM : kPointN;
  if (EC_POINT_oct2point(group, point, encoded.data(), encoded.size(), context) != 1 ||
      EC_POINT_is_on_curve(group, point, context) != 1 ||
      EC_POINT_is_at_infinity(group, point) == 1) {
    return Error::CryptoFailure;
  }
  return Error::None;
}

Error serializePoint(const EC_GROUP* group, const EC_POINT* point, BN_CTX* context,
                     Point65& encoded) {
  const size_t written = EC_POINT_point2oct(
      group, point, POINT_CONVERSION_UNCOMPRESSED, encoded.data(), encoded.size(), context);
  return written == encoded.size() && encoded[0] == 0x04
             ? Error::None
             : Error::CryptoFailure;
}

Error parsePeerPoint(const EC_GROUP* group, const Point65& encoded, EC_POINT* point,
                     BN_CTX* context) {
  if (encoded[0] != 0x04 ||
      EC_POINT_oct2point(group, point, encoded.data(), encoded.size(), context) != 1 ||
      EC_POINT_is_at_infinity(group, point) == 1 ||
      EC_POINT_is_on_curve(group, point, context) != 1) {
    return Error::InvalidPoint;
  }
  Point orderCheck(EC_POINT_new(group));
  if (!orderCheck ||
      EC_POINT_mul(group, orderCheck.get(), nullptr, point,
                   EC_GROUP_get0_order(group), context) != 1 ||
      EC_POINT_is_at_infinity(group, orderCheck.get()) != 1) {
    return Error::InvalidPoint;
  }
  return Error::None;
}

Error validateScalar(const EC_GROUP* group, const Key32& encoded,
                     BigNumber& scalar) {
  scalar.reset(BN_secure_new());
  if (!scalar || BN_bin2bn(encoded.data(), static_cast<int>(encoded.size()),
                           scalar.get()) == nullptr ||
      BN_is_zero(scalar.get()) == 1 || BN_is_negative(scalar.get()) == 1 ||
      BN_cmp(scalar.get(), EC_GROUP_get0_order(group)) >= 0) {
    scalar.reset();
    return Error::InvalidArgument;
  }
  return Error::None;
}

Bytes pairingAad(const Salt16& salt) {
  Bytes aad(kPairingDomain.begin(), kPairingDomain.end());
  aad.insert(aad.end(), salt.begin(), salt.end());
  return aad;
}

}  // namespace

struct Spake2P256::Impl {
  Impl(SpakeRole selectedRole, std::string firstIdentity, std::string secondIdentity,
       Bytes associatedData, Group selectedGroup, BigNumber password,
       BigNumber ephemeral, Point65 encodedPublic)
      : role(selectedRole), identityA(std::move(firstIdentity)),
        identityB(std::move(secondIdentity)), aad(std::move(associatedData)),
        group(std::move(selectedGroup)), passwordScalar(std::move(password)),
        privateScalar(std::move(ephemeral)), publicValue(encodedPublic) {}

  ~Impl() {
    if (passwordScalar) BN_clear(passwordScalar.get());
    if (privateScalar) BN_clear(privateScalar.get());
    detail::secureClear(aad.data(), aad.size());
  }

  SpakeRole role;
  std::string identityA;
  std::string identityB;
  Bytes aad;
  Group group;
  BigNumber passwordScalar;
  BigNumber privateScalar;
  Point65 publicValue{};
  bool finished = false;
};

Spake2P256::Spake2P256() = default;
Spake2P256::~Spake2P256() = default;
Spake2P256::Spake2P256(Spake2P256&&) noexcept = default;
Spake2P256& Spake2P256::operator=(Spake2P256&&) noexcept = default;
Spake2P256::Spake2P256(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Error Spake2P256::create(SpakeRole role, std::string identityA,
                         std::string identityB, Bytes aad,
                         const Key32& passwordScalar, Spake2P256& out) {
  Group group(EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1));
  BigNumberContext context(BN_CTX_new());
  BigNumber ephemeral(BN_secure_new());
  if (!group || !context || !ephemeral) return Error::CryptoFailure;
  do {
    if (BN_priv_rand_range_ex(ephemeral.get(), EC_GROUP_get0_order(group.get()), 256,
                              context.get()) != 1) {
      return Error::CryptoFailure;
    }
  } while (BN_is_zero(ephemeral.get()) == 1);
  Key32 encoded{};
  if (BN_bn2binpad(ephemeral.get(), encoded.data(), static_cast<int>(encoded.size())) !=
      static_cast<int>(encoded.size())) {
    return Error::CryptoFailure;
  }
  const Error error = createDeterministic(
      role, std::move(identityA), std::move(identityB), std::move(aad),
      passwordScalar, encoded, out);
  detail::secureClear(encoded.data(), encoded.size());
  return error;
}

Error Spake2P256::createDeterministic(
    SpakeRole role, std::string identityA, std::string identityB, Bytes aad,
    const Key32& passwordScalar, const Key32& privateScalar, Spake2P256& out) {
  if (identityA.size() > 1024 || identityB.size() > 1024 || aad.size() > 4096) {
    return Error::InvalidArgument;
  }
  Group group(EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1));
  BigNumberContext context(BN_CTX_new());
  if (!group || !context) return Error::CryptoFailure;
  BigNumber password;
  BigNumber ephemeral;
  Error error = validateScalar(group.get(), passwordScalar, password);
  if (error != Error::None) return error;
  error = validateScalar(group.get(), privateScalar, ephemeral);
  if (error != Error::None) return error;

  Point mask(EC_POINT_new(group.get()));
  Point publicPoint(EC_POINT_new(group.get()));
  if (!mask || !publicPoint) return Error::CryptoFailure;
  error = loadMaskPoint(group.get(), role, mask.get(), context.get());
  if (error != Error::None ||
      EC_POINT_mul(group.get(), publicPoint.get(), ephemeral.get(), mask.get(),
                   password.get(), context.get()) != 1 ||
      EC_POINT_is_at_infinity(group.get(), publicPoint.get()) == 1) {
    return Error::CryptoFailure;
  }
  Point65 encodedPublic{};
  error = serializePoint(group.get(), publicPoint.get(), context.get(), encodedPublic);
  if (error != Error::None) return error;

  try {
    out = Spake2P256(std::make_unique<Impl>(
        role, std::move(identityA), std::move(identityB), std::move(aad),
        std::move(group), std::move(password), std::move(ephemeral), encodedPublic));
  } catch (...) {
    return Error::CryptoFailure;
  }
  return Error::None;
}

const Point65& Spake2P256::publicValue() const {
  static const Point65 empty{};
  return impl_ ? impl_->publicValue : empty;
}

Error Spake2P256::finish(const Point65& peerPublicValue, Spake2Secrets& out) {
  if (!impl_ || impl_->finished) return Error::InvalidArgument;
  BigNumberContext context(BN_CTX_new());
  Point peer(EC_POINT_new(impl_->group.get()));
  Point mask(EC_POINT_new(impl_->group.get()));
  Point passwordMask(EC_POINT_new(impl_->group.get()));
  Point unmasked(EC_POINT_new(impl_->group.get()));
  Point shared(EC_POINT_new(impl_->group.get()));
  if (!context || !peer || !mask || !passwordMask || !unmasked || !shared) {
    return Error::CryptoFailure;
  }
  Error error = parsePeerPoint(impl_->group.get(), peerPublicValue, peer.get(), context.get());
  if (error != Error::None) return error;
  const SpakeRole peerMaskRole = impl_->role == SpakeRole::A ? SpakeRole::B : SpakeRole::A;
  error = loadMaskPoint(impl_->group.get(), peerMaskRole, mask.get(), context.get());
  if (error != Error::None ||
      EC_POINT_mul(impl_->group.get(), passwordMask.get(), nullptr, mask.get(),
                   impl_->passwordScalar.get(), context.get()) != 1 ||
      EC_POINT_invert(impl_->group.get(), passwordMask.get(), context.get()) != 1 ||
      EC_POINT_add(impl_->group.get(), unmasked.get(), peer.get(), passwordMask.get(),
                   context.get()) != 1 ||
      EC_POINT_is_at_infinity(impl_->group.get(), unmasked.get()) == 1 ||
      EC_POINT_mul(impl_->group.get(), shared.get(), nullptr, unmasked.get(),
                   impl_->privateScalar.get(), context.get()) != 1 ||
      EC_POINT_is_at_infinity(impl_->group.get(), shared.get()) == 1) {
    return Error::InvalidPoint;
  }

  Spake2Secrets secrets;
  error = serializePoint(impl_->group.get(), shared.get(), context.get(),
                         secrets.sharedPoint);
  if (error != Error::None) return error;
  const Point65& publicA = impl_->role == SpakeRole::A
                               ? impl_->publicValue
                               : peerPublicValue;
  const Point65& publicB = impl_->role == SpakeRole::B
                               ? impl_->publicValue
                               : peerPublicValue;
  Key32 encodedPassword{};
  if (BN_bn2binpad(impl_->passwordScalar.get(), encodedPassword.data(),
                   static_cast<int>(encodedPassword.size())) !=
      static_cast<int>(encodedPassword.size())) {
    return Error::CryptoFailure;
  }
  detail::appendLengthPrefixed(secrets.transcript, impl_->identityA);
  detail::appendLengthPrefixed(secrets.transcript, impl_->identityB);
  detail::appendLengthPrefixed(secrets.transcript, publicA.data(), publicA.size());
  detail::appendLengthPrefixed(secrets.transcript, publicB.data(), publicB.size());
  detail::appendLengthPrefixed(secrets.transcript, secrets.sharedPoint.data(),
                               secrets.sharedPoint.size());
  detail::appendLengthPrefixed(secrets.transcript, encodedPassword.data(),
                               encodedPassword.size());
  detail::secureClear(encodedPassword.data(), encodedPassword.size());

  Key32 transcriptHash{};
  error = detail::sha256(secrets.transcript.data(), secrets.transcript.size(),
                         transcriptHash);
  if (error != Error::None) return error;
  std::copy_n(transcriptHash.data(), secrets.sharedSecret.size(),
              secrets.sharedSecret.begin());
  Key16 authenticationKey{};
  std::copy_n(transcriptHash.data() + secrets.sharedSecret.size(),
              authenticationKey.size(), authenticationKey.begin());
  detail::secureClear(transcriptHash.data(), transcriptHash.size());

  constexpr std::string_view confirmationLabel = "ConfirmationKeys";
  Bytes confirmationInfo(confirmationLabel.begin(), confirmationLabel.end());
  confirmationInfo.insert(confirmationInfo.end(), impl_->aad.begin(), impl_->aad.end());
  std::array<uint8_t, 32> confirmationMaterial{};
  error = detail::hkdfSha256(
      authenticationKey.data(), authenticationKey.size(), nullptr, 0,
      confirmationInfo.data(), confirmationInfo.size(), confirmationMaterial.data(),
      confirmationMaterial.size());
  detail::secureClear(authenticationKey.data(), authenticationKey.size());
  detail::secureClear(confirmationInfo.data(), confirmationInfo.size());
  if (error != Error::None) return error;
  std::copy_n(confirmationMaterial.data(), secrets.confirmationKeyA.size(),
              secrets.confirmationKeyA.begin());
  std::copy_n(confirmationMaterial.data() + secrets.confirmationKeyA.size(),
              secrets.confirmationKeyB.size(), secrets.confirmationKeyB.begin());
  detail::secureClear(confirmationMaterial.data(), confirmationMaterial.size());

  error = detail::hmacSha256(
      secrets.confirmationKeyA.data(), secrets.confirmationKeyA.size(),
      secrets.transcript.data(), secrets.transcript.size(), secrets.confirmationA);
  if (error == Error::None) {
    error = detail::hmacSha256(
        secrets.confirmationKeyB.data(), secrets.confirmationKeyB.size(),
        secrets.transcript.data(), secrets.transcript.size(), secrets.confirmationB);
  }
  if (error != Error::None) return error;
  impl_->finished = true;
  BN_clear(impl_->passwordScalar.get());
  BN_clear(impl_->privateScalar.get());
  out = std::move(secrets);
  return Error::None;
}

Error derivePasswordScalar(std::string_view sixDigitCode, const Salt16& salt,
                           Key32& scalar) {
  if (sixDigitCode.size() != 6 ||
      !std::all_of(sixDigitCode.begin(), sixDigitCode.end(),
                   [](char character) { return character >= '0' && character <= '9'; })) {
    return Error::InvalidArgument;
  }
  Kdf algorithm(EVP_KDF_fetch(nullptr, "SCRYPT", nullptr));
  KdfContext context(algorithm ? EVP_KDF_CTX_new(algorithm.get()) : nullptr);
  if (!algorithm || !context) return Error::CryptoFailure;
  uint64_t n = 32768;
  uint64_t r = 8;
  uint64_t p = 1;
  uint64_t maxMemory = 64ull * 1024ull * 1024ull;
  OSSL_PARAM parameters[] = {
      OSSL_PARAM_construct_octet_string(
          OSSL_KDF_PARAM_PASSWORD,
          const_cast<char*>(sixDigitCode.data()), sixDigitCode.size()),
      OSSL_PARAM_construct_octet_string(
          OSSL_KDF_PARAM_SALT, const_cast<uint8_t*>(salt.data()), salt.size()),
      OSSL_PARAM_construct_uint64(OSSL_KDF_PARAM_SCRYPT_N, &n),
      OSSL_PARAM_construct_uint64(OSSL_KDF_PARAM_SCRYPT_R, &r),
      OSSL_PARAM_construct_uint64(OSSL_KDF_PARAM_SCRYPT_P, &p),
      OSSL_PARAM_construct_uint64(OSSL_KDF_PARAM_SCRYPT_MAXMEM, &maxMemory),
      OSSL_PARAM_construct_end()};
  std::array<uint8_t, 40> stretched{};
  if (EVP_KDF_derive(context.get(), stretched.data(), stretched.size(), parameters) != 1) {
    return Error::CryptoFailure;
  }

  Group group(EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1));
  BigNumberContext numberContext(BN_CTX_new());
  BigNumber wide(BN_bin2bn(stretched.data(), static_cast<int>(stretched.size()), nullptr));
  BigNumber reduced(BN_secure_new());
  detail::secureClear(stretched.data(), stretched.size());
  if (!group || !numberContext || !wide || !reduced ||
      BN_nnmod(reduced.get(), wide.get(), EC_GROUP_get0_order(group.get()),
               numberContext.get()) != 1 ||
      BN_is_zero(reduced.get()) == 1 ||
      BN_bn2binpad(reduced.get(), scalar.data(), static_cast<int>(scalar.size())) !=
          static_cast<int>(scalar.size())) {
    detail::secureClear(scalar.data(), scalar.size());
    return Error::CryptoFailure;
  }
  return Error::None;
}

struct PairingServer::Impl {
  Impl(std::string serverId, std::string phoneId, PairingOffer pairingOffer,
       Spake2P256 exchange)
      : serviceId(std::move(serverId)), deviceId(std::move(phoneId)),
        offer(std::move(pairingOffer)), spake(std::move(exchange)) {}

  ~Impl() {
    if (secrets.has_value()) {
      detail::secureClear(secrets->sharedSecret.data(), secrets->sharedSecret.size());
      detail::secureClear(secrets->confirmationKeyA.data(),
                          secrets->confirmationKeyA.size());
      detail::secureClear(secrets->confirmationKeyB.data(),
                          secrets->confirmationKeyB.size());
    }
  }

  std::string serviceId;
  std::string deviceId;
  PairingOffer offer;
  Spake2P256 spake;
  std::optional<Spake2Secrets> secrets;
  bool verified = false;
};

PairingServer::PairingServer() = default;
PairingServer::~PairingServer() = default;
PairingServer::PairingServer(PairingServer&&) noexcept = default;
PairingServer& PairingServer::operator=(PairingServer&&) noexcept = default;
PairingServer::PairingServer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Error PairingServer::begin(std::string serviceId, std::string deviceId,
                           std::string_view sixDigitCode, uint64_t nowUnixSeconds,
                           PairingServer& out) {
  if (!detail::validCanonicalId(serviceId) ||
      !detail::validCanonicalId(deviceId) ||
      nowUnixSeconds > std::numeric_limits<uint64_t>::max() - kPairingLifetimeSeconds) {
    return Error::InvalidArgument;
  }
  PairingOffer offer;
  Error error = detail::randomBytes(offer.salt.data(), offer.salt.size());
  if (error != Error::None) return error;
  offer.expiresUnixSeconds = nowUnixSeconds + kPairingLifetimeSeconds;
  Key32 password{};
  error = derivePasswordScalar(sixDigitCode, offer.salt, password);
  if (error != Error::None) return error;
  Spake2P256 exchange;
  error = Spake2P256::create(
      SpakeRole::B, deviceId, serviceId, pairingAad(offer.salt), password, exchange);
  detail::secureClear(password.data(), password.size());
  if (error != Error::None) return error;
  offer.publicValueB = exchange.publicValue();
  try {
    out = PairingServer(std::make_unique<Impl>(
        std::move(serviceId), std::move(deviceId), offer, std::move(exchange)));
  } catch (...) {
    return Error::CryptoFailure;
  }
  return Error::None;
}

const PairingOffer& PairingServer::offer() const {
  static const PairingOffer empty{};
  return impl_ ? impl_->offer : empty;
}

Error PairingServer::receiveCommit(const Point65& publicValueA,
                                   Key32& confirmationB) {
  if (!impl_ || impl_->secrets.has_value()) return Error::InvalidArgument;
  Spake2Secrets secrets;
  const Error error = impl_->spake.finish(publicValueA, secrets);
  if (error != Error::None) return error;
  confirmationB = secrets.confirmationB;
  impl_->secrets = std::move(secrets);
  return Error::None;
}

Error PairingServer::verifyPeer(const Key32& confirmationA,
                                uint64_t nowUnixSeconds, PairingRecord& record) {
  if (!impl_ || !impl_->secrets.has_value() || impl_->verified) {
    return Error::InvalidArgument;
  }
  if (nowUnixSeconds >= impl_->offer.expiresUnixSeconds) return Error::Expired;
  if (!detail::constantTimeEqual(confirmationA.data(),
                                 impl_->secrets->confirmationA.data(),
                                 confirmationA.size())) {
    return Error::AuthenticationFailed;
  }
  Bytes info(kRecordDomain.begin(), kRecordDomain.end());
  detail::appendLengthPrefixed(info, impl_->deviceId);  // role A / phone
  detail::appendLengthPrefixed(info, impl_->serviceId); // role B / PC
  PairingRecord paired;
  paired.serviceId = impl_->serviceId;
  paired.deviceId = impl_->deviceId;
  paired.createdUnixSeconds = nowUnixSeconds;
  paired.expiresUnixSeconds = std::numeric_limits<uint64_t>::max();
  const Error error = detail::hkdfSha256(
      impl_->secrets->sharedSecret.data(), impl_->secrets->sharedSecret.size(),
      impl_->offer.salt.data(), impl_->offer.salt.size(), info.data(), info.size(),
      paired.key.data(), paired.key.size());
  detail::secureClear(info.data(), info.size());
  if (error != Error::None) return error;
  impl_->verified = true;
  record = paired;
  detail::secureClear(paired.key.data(), paired.key.size());
  return Error::None;
}

}  // namespace rcsecurity
