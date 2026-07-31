#include "rcsecurity/security.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const char* what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::printf("  FAIL: %s\n", what);
  }
}

uint8_t nibble(char character) {
  if (character >= '0' && character <= '9') {
    return static_cast<uint8_t>(character - '0');
  }
  return static_cast<uint8_t>(character - 'a' + 10);
}

template <size_t Size>
std::array<uint8_t, Size> fromHex(std::string_view hex) {
  std::array<uint8_t, Size> out{};
  if (hex.size() != Size * 2) return out;
  for (size_t index = 0; index < Size; ++index) {
    out[index] = static_cast<uint8_t>((nibble(hex[index * 2]) << 4) |
                                      nibble(hex[index * 2 + 1]));
  }
  return out;
}

template <size_t Size>
bool equalsHex(const std::vector<uint8_t>& value, std::string_view hex) {
  const auto expected = fromHex<Size>(hex);
  return value.size() == expected.size() &&
         std::equal(value.begin(), value.end(), expected.begin());
}

bool channelMatches(const rcsecurity::ChannelKeys& value, std::string_view hex) {
  const auto expected = fromHex<36>(hex);
  return std::equal(value.key.begin(), value.key.end(), expected.begin()) &&
         std::equal(value.noncePrefix.begin(), value.noncePrefix.end(),
                    expected.begin() + static_cast<std::ptrdiff_t>(value.key.size()));
}

uint64_t unixNow() {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(now).count());
}

std::filesystem::path temporaryDirectory() {
  wchar_t root[MAX_PATH]{};
  const DWORD length = ::GetTempPathW(MAX_PATH, root);
  if (length == 0 || length >= MAX_PATH) return {};
  return std::filesystem::path(root) /
         (L"RemoteCam-security-test-" + std::to_wstring(::GetCurrentProcessId()) +
          L"-" + std::to_wstring(::GetTickCount64()));
}

void testRatePolicy() {
  std::printf("Attempt policy\n");
  using Clock = rcsecurity::AttemptPolicy::TimePoint::clock;
  const auto start = Clock::now();

  rcsecurity::AttemptPolicyConfig perSourceConfig;
  perSourceConfig.maxAttemptsGlobal = 100;
  rcsecurity::AttemptPolicy perSource(perSourceConfig);
  std::array<rcsecurity::AttemptPolicy::AttemptId, 5> attempts{};
  for (size_t index = 0; index < attempts.size(); ++index) {
    check(perSource.beginAttempt("192.0.2.10", start, attempts[index]) ==
              rcsecurity::Error::None,
          "five per-source attempts are admitted");
  }
  rcsecurity::AttemptPolicy::AttemptId blocked = 0;
  check(perSource.beginAttempt("192.0.2.10", start, blocked) ==
            rcsecurity::Error::RateLimited,
        "sixth per-source attempt is blocked");
  perSource.finishAttempt(attempts.back(), true, start);
  check(perSource.beginAttempt("192.0.2.10", start, blocked) ==
            rcsecurity::Error::None,
        "successful completion refunds its provisional charge");

  rcsecurity::AttemptPolicyConfig globalConfig;
  globalConfig.maxAttemptsPerSource = 100;
  rcsecurity::AttemptPolicy global(globalConfig);
  for (size_t index = 0; index < 5; ++index) {
    rcsecurity::AttemptPolicy::AttemptId id = 0;
    check(global.beginAttempt("source-" + std::to_string(index), start, id) ==
              rcsecurity::Error::None,
          "five distinct-source attempts are globally admitted");
  }
  check(global.beginAttempt("source-6", start, blocked) ==
            rcsecurity::Error::RateLimited,
        "unfinished attempts count against the global limit");
  check(!global.allowed("source-6", start + std::chrono::seconds(299)),
        "global cooldown remains active just before five minutes");
  check(global.allowed("source-6", start + std::chrono::seconds(300)),
        "global cooldown expires exactly at five minutes");

  rcsecurity::AttemptPolicy defaults;
  check(!defaults.pairingExpired(start, start + std::chrono::seconds(119)) &&
            defaults.pairingExpired(start, start + std::chrono::seconds(120)),
        "pairing TTL boundary is exactly two minutes");
  check(!defaults.authenticationExpired(start, start + std::chrono::seconds(9)) &&
            defaults.authenticationExpired(start, start + std::chrono::seconds(10)),
        "authentication TTL boundary is exactly ten seconds");
}

void testPairingStore() {
  std::printf("DPAPI pairing store\n");
  const std::filesystem::path directory = temporaryDirectory();
  rcsecurity::PairingStore store(directory, "0123456789abcdef");
  rcsecurity::PairingRecord record;
  record.serviceId = "0123456789abcdef";
  record.deviceId = "fedcba9876543210";
  for (size_t index = 0; index < record.key.size(); ++index) {
    record.key[index] = static_cast<uint8_t>(index + 1);
  }
  record.createdUnixSeconds = 100;
  record.expiresUnixSeconds = std::numeric_limits<uint64_t>::max();
  check(store.save(record) == rcsecurity::Error::None,
        "DPAPI record is atomically saved");
  rcsecurity::PairingRecord loaded;
  check(store.load(record.deviceId, loaded) == rcsecurity::Error::None &&
            loaded.serviceId == record.serviceId && loaded.deviceId == record.deviceId &&
            loaded.key == record.key &&
            loaded.expiresUnixSeconds == std::numeric_limits<uint64_t>::max(),
        "DPAPI record round-trips with the non-expiring sentinel");

  size_t temporaryFiles = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory)) {
    if (entry.path().extension() != L".rcpair") ++temporaryFiles;
  }
  check(temporaryFiles == 0, "atomic save leaves no temporary record behind");

  rcsecurity::PairingStore wrongService(directory, "1111111111111111");
  check(wrongService.load(record.deviceId, loaded) == rcsecurity::Error::CorruptRecord,
        "DPAPI entropy binds a record to the PC service identity");
  check(store.erase(record.deviceId) == rcsecurity::Error::None &&
            store.load(record.deviceId, loaded) == rcsecurity::Error::NotFound,
        "unpair erases the long-term record");
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

#if RCSECURITY_OPENSSL_AVAILABLE

void testRfc9382Vector() {
  std::printf("RFC 9382 SPAKE2 vector\n");
  const rcsecurity::Key32 w = fromHex<32>(
      "2ee57912099d31560b3a44b1184b9b4866e904c49d12ac5042c97dca461b1a5f");
  const rcsecurity::Key32 x = fromHex<32>(
      "43dd0fd7215bdcb482879fca3220c6a968e66d70b1356cac18bb26c84a78d729");
  const rcsecurity::Key32 y = fromHex<32>(
      "dcb60106f276b02606d8ef0a328c02e4b629f84f89786af5befb0bc75b6e66be");
  const rcsecurity::Point65 expectedA = fromHex<65>(
      "04a56fa807caaa53a4d28dbb9853b9815c61a411118a6fe516a8798434751470f9"
      "010153ac33d0d5f2047ffdb1a3e42c9b4e6be662766e1eeb4116988ede5f912c");
  const rcsecurity::Point65 expectedB = fromHex<65>(
      "0406557e482bd03097ad0cbaa5df82115460d951e3451962f1eaf4367a420676d0"
      "9857ccbc522686c83d1852abfa8ed6e4a1155cf8f1543ceca528afb591a1e0b7");
  const rcsecurity::Point65 expectedShared = fromHex<65>(
      "0412af7e89717850671913e6b469ace67bd90a4df8ce45c2af19010175e37eed69"
      "f75897996d539356e2fa6a406d528501f907e04d97515fbe83db277b715d3325");
  const rcsecurity::Key16 expectedKe =
      fromHex<16>("0e0672dc86f8e45565d338b0540abe69");
  const rcsecurity::Key16 expectedKcA =
      fromHex<16>("00c12546835755c86d8c0db7851ae86f");
  const rcsecurity::Key16 expectedKcB =
      fromHex<16>("a9fa3406c3b781b93d804485430ca27a");
  const rcsecurity::Key32 expectedConfirmationA = fromHex<32>(
      "58ad4aa88e0b60d5061eb6b5dd93e80d9c4f00d127c65b3b35b1b5281fee38f0");
  const rcsecurity::Key32 expectedConfirmationB = fromHex<32>(
      "d3e2e547f1ae04f2dbdbf0fc4b79f8ecff2dff314b5d32fe9fcef2fb26dc459b");

  rcsecurity::Spake2P256 roleA;
  rcsecurity::Spake2P256 roleB;
  check(rcsecurity::Spake2P256::createDeterministic(
            rcsecurity::SpakeRole::A, "server", "client", {}, w, x, roleA) ==
            rcsecurity::Error::None &&
            roleA.publicValue() == expectedA,
        "role A uses RFC M and emits the published pA");
  check(rcsecurity::Spake2P256::createDeterministic(
            rcsecurity::SpakeRole::B, "server", "client", {}, w, y, roleB) ==
            rcsecurity::Error::None &&
            roleB.publicValue() == expectedB,
        "role B uses RFC N and emits the published pB");
  rcsecurity::Spake2Secrets secretsA;
  rcsecurity::Spake2Secrets secretsB;
  check(roleA.finish(roleB.publicValue(), secretsA) == rcsecurity::Error::None &&
            roleB.finish(roleA.publicValue(), secretsB) == rcsecurity::Error::None,
        "both RFC roles finish");
  check(secretsA.sharedPoint == expectedShared &&
            secretsB.sharedPoint == expectedShared &&
            secretsA.sharedSecret == expectedKe,
        "RFC transcript yields the published shared point and Ke");
  check(secretsA.confirmationKeyA == expectedKcA &&
            secretsA.confirmationKeyB == expectedKcB &&
            secretsA.confirmationA == expectedConfirmationA &&
            secretsA.confirmationB == expectedConfirmationB &&
            secretsA.confirmationA == secretsB.confirmationA &&
            secretsA.confirmationB == secretsB.confirmationB,
        "RFC HKDF/HMAC key-confirmation values match exactly");

  rcsecurity::Point65 invalid = expectedB;
  invalid[0] = 0x05;
  rcsecurity::Spake2P256 invalidPeer;
  check(rcsecurity::Spake2P256::createDeterministic(
            rcsecurity::SpakeRole::A, "server", "client", {}, w, x, invalidPeer) ==
            rcsecurity::Error::None &&
            invalidPeer.finish(invalid, secretsA) == rcsecurity::Error::InvalidPoint,
        "non-SEC1 or off-curve peers fail closed");
}

void testPairingRolesAndScrypt() {
  std::printf("Pairing roles and scrypt\n");
  constexpr std::string_view serviceId = "0123456789abcdef";
  constexpr std::string_view deviceId = "fedcba9876543210";
  constexpr std::string_view code = "007349";
  rcsecurity::Salt16 fixedSalt{};
  for (size_t index = 0; index < fixedSalt.size(); ++index) {
    fixedSalt[index] = static_cast<uint8_t>(index);
  }
  rcsecurity::Key32 fixedPassword{};
  check(rcsecurity::derivePasswordScalar(code, fixedSalt, fixedPassword) ==
            rcsecurity::Error::None &&
            fixedPassword == fromHex<32>(
                "3fdb49ed1a73b5b2528d7ae3f4f3bfcfe9d9ea8221ec774e3b843eb9010fc9b4"),
        "scrypt/reduction matches the deterministic project vector");
  const uint64_t now = unixNow();
  rcsecurity::PairingServer server;
  check(rcsecurity::PairingServer::begin(std::string(serviceId), std::string(deviceId),
                                         code, now, server) == rcsecurity::Error::None,
        "PC pairing server begins as role B");
  rcsecurity::Key32 password{};
  check(rcsecurity::derivePasswordScalar(code, server.offer().salt, password) ==
            rcsecurity::Error::None,
        "six-digit code is stretched by the fixed scrypt profile");
  std::vector<uint8_t> aad{'R','e','m','o','t','e','C','a','m',' ',
                           'S','P','A','K','E','2',' ','p','a','i','r','i','n','g',' ','v','1'};
  aad.insert(aad.end(), server.offer().salt.begin(), server.offer().salt.end());
  rcsecurity::Spake2P256 phone;
  check(rcsecurity::Spake2P256::create(rcsecurity::SpakeRole::A,
                                       std::string(deviceId), std::string(serviceId),
                                       aad, password, phone) == rcsecurity::Error::None,
        "phone pairing client is role A");
  rcsecurity::Spake2Secrets phoneSecrets;
  check(phone.finish(server.offer().publicValueB, phoneSecrets) == rcsecurity::Error::None,
        "phone consumes the PC role-B public value");
  rcsecurity::Key32 confirmationB{};
  check(server.receiveCommit(phone.publicValue(), confirmationB) == rcsecurity::Error::None &&
            confirmationB == phoneSecrets.confirmationB,
        "PC returns role-B key confirmation");
  rcsecurity::PairingRecord record;
  check(server.verifyPeer(phoneSecrets.confirmationA, now + 1, record) ==
            rcsecurity::Error::None &&
            record.expiresUnixSeconds == std::numeric_limits<uint64_t>::max(),
        "PC verifies role A and creates a non-expiring long-term key");
  rcsecurity::PairingServer expired;
  check(rcsecurity::PairingServer::begin(std::string(serviceId), std::string(deviceId),
                                         code, now, expired) == rcsecurity::Error::None,
        "expiry fixture begins");
  rcsecurity::Spake2P256 expiredPhone;
  check(rcsecurity::derivePasswordScalar(code, expired.offer().salt, password) ==
            rcsecurity::Error::None,
        "expiry fixture derives password");
  aad.assign({'R','e','m','o','t','e','C','a','m',' ',
              'S','P','A','K','E','2',' ','p','a','i','r','i','n','g',' ','v','1'});
  aad.insert(aad.end(), expired.offer().salt.begin(), expired.offer().salt.end());
  check(rcsecurity::Spake2P256::create(rcsecurity::SpakeRole::A,
                                       std::string(deviceId), std::string(serviceId),
                                       aad, password, expiredPhone) == rcsecurity::Error::None &&
            expiredPhone.finish(expired.offer().publicValueB, phoneSecrets) ==
                rcsecurity::Error::None &&
            expired.receiveCommit(expiredPhone.publicValue(), confirmationB) ==
                rcsecurity::Error::None &&
            expired.verifyPeer(phoneSecrets.confirmationA, now + 120, record) ==
                rcsecurity::Error::Expired,
        "unfinished PAKE expires exactly at two minutes");
}

void testAuthenticationAndEnvelopes() {
  std::printf("Authentication and envelopes\n");
  constexpr std::string_view serviceId = "0123456789abcdef";
  constexpr std::string_view deviceId = "fedcba9876543210";
  rcsecurity::Key32 pairingKey{};
  for (size_t index = 0; index < pairingKey.size(); ++index) {
    pairingKey[index] = static_cast<uint8_t>(index * 3 + 1);
  }
  rcsecurity::Nonce32 serverNonce{};
  rcsecurity::Nonce32 clientNonce{};
  for (size_t index = 0; index < serverNonce.size(); ++index) {
    serverNonce[index] = static_cast<uint8_t>(index);
    clientNonce[index] = static_cast<uint8_t>(0xffu - index);
  }
  rcsecurity::Key32 clientProof{};
  rcsecurity::Key32 serverProof{};
  check(rcsecurity::makeAuthenticationProof(pairingKey, false, serviceId, deviceId,
                                             serverNonce, clientNonce, clientProof) ==
            rcsecurity::Error::None &&
            rcsecurity::makeAuthenticationProof(pairingKey, true, serviceId, deviceId,
                                                 serverNonce, clientNonce, serverProof) ==
                rcsecurity::Error::None &&
            clientProof == fromHex<32>(
                "6e0f03a5b0311cbccfe9606a6bc31311a6dfd823102e7a111514f6556d1422bb") &&
            serverProof == fromHex<32>(
                "9d760b31f84b8724feb5387ba5806056947ef19bdd29b734f3058ce170baa056"),
        "client/server proof domains match deterministic HMAC vectors");

  rcsecurity::SessionKeys keys{};
  check(rcsecurity::deriveSessionKeys(pairingKey, serviceId, deviceId, serverNonce,
                                      clientNonce, keys) == rcsecurity::Error::None,
        "session keys derive deterministically");
  check(keys.controlAtoB.key != keys.controlBtoA.key &&
            keys.videoAtoB.key != keys.videoBtoA.key &&
            keys.statsAtoB.key != keys.statsBtoA.key &&
            keys.controlAtoB.key != keys.videoAtoB.key &&
            keys.videoAtoB.key != keys.statsAtoB.key &&
            keys.videoAtoB.noncePrefix != keys.videoBtoA.noncePrefix &&
            keys.videoAtoB.noncePrefix != keys.statsAtoB.noncePrefix,
        "direction/channel keys and nonce prefixes are distinct");
  check(channelMatches(
            keys.controlAtoB,
            "4504dbe91d18e2097cc86f2a104bccecd8ca15185b17b23c531b50aaeb96b3cfe338d71a") &&
            channelMatches(
                keys.controlBtoA,
                "e616c70cad945a24e62942d8861ba6de27cc4f22601c6a56af83312f9365a995e2818e9c") &&
            channelMatches(
                keys.videoAtoB,
                "336cfc7164808f58f3f3e011ee8302c3224eb56b561dbfa944db0e26e634a3b04245a7d7") &&
            channelMatches(
                keys.videoBtoA,
                "5f11b319db073f27e5635970cd2a4c64e04ac7ba457d1c8617b0bf781781cb63ed812616") &&
            channelMatches(
                keys.statsAtoB,
                "5c781ed0d72bdcd54ae3146a7be3a65944edabf08ce6833c4e06b1f044514adbaeac8fce") &&
            channelMatches(
                keys.statsBtoA,
                "549fd8c2fe502431fd3b2eb4497ab208be85f9bfb2b8be0ece1f13986dc738bbfa12819c"),
        "session HKDF matches all six deterministic key/prefix records");

  const uint64_t now = unixNow();
  std::shared_ptr<rcsecurity::ISessionProtector> client;
  std::shared_ptr<rcsecurity::ISessionProtector> server;
  check(rcsecurity::createSessionProtector(rcsecurity::SessionRole::Client, keys, now,
                                            client) == rcsecurity::Error::None &&
            rcsecurity::createSessionProtector(rcsecurity::SessionRole::Server, keys, now,
                                                server) == rcsecurity::Error::None &&
            client->expiresUnixSeconds() ==
                now + rcsecurity::kMaximumSessionLifetimeSeconds,
        "matching client/server protectors have a 24-hour deadline");

  const std::vector<uint8_t> control{'h','e','l','l','o'};
  rcsecurity::ProtectedPayload protectedControl;
  std::vector<uint8_t> recovered;
  check(client->protectControl(0, 0, 17, control.data(), control.size(),
                               protectedControl) == rcsecurity::Error::None &&
            equalsHex<45>(
                protectedControl.payload,
                "000000000000000068656c6c6fb10d9013487c341fec1e2615bff374da64da6c915ea2cb56b996560569d0f13c") &&
            server->unprotectControl(0, protectedControl.flags, 17,
                                     protectedControl.payload.data(),
                                     protectedControl.payload.size(), recovered) ==
                rcsecurity::Error::None &&
            recovered == control,
        "control envelope authenticates full-header AAD and payload");
  check(server->unprotectControl(0, protectedControl.flags, 17,
                                 protectedControl.payload.data(),
                                 protectedControl.payload.size(), recovered) ==
            rcsecurity::Error::SequenceMismatch,
        "control replay is rejected by the strict sequence");

  const std::vector<uint8_t> video{0, 0, 0, 1, 0x65, 0x01};
  rcsecurity::ProtectedPayload protectedVideo;
  check(client->protectMedia(1, 1, 99, video.data(), video.size(), protectedVideo) ==
            rcsecurity::Error::None &&
            (protectedVideo.flags & 0x02u) != 0 &&
            equalsHex<30>(
                protectedVideo.payload,
                "0000000000000000077daeb827fc3666aad1956d63c73fbd2d48e61a0c77"),
        "video matches the deterministic ChaCha20-Poly1305 envelope vector");
  std::vector<uint8_t> tampered = protectedVideo.payload;
  tampered.back() ^= 1;
  check(server->unprotectMedia(1, protectedVideo.flags, 99, tampered.data(),
                               tampered.size(), recovered) ==
            rcsecurity::Error::AuthenticationFailed,
        "tampered AEAD tag is rejected without advancing sequence");
  check(server->unprotectMedia(1, protectedVideo.flags, 100,
                               protectedVideo.payload.data(),
                               protectedVideo.payload.size(), recovered) ==
            rcsecurity::Error::AuthenticationFailed,
        "changed PTS in full-header AAD is rejected");
  check(server->unprotectMedia(1, protectedVideo.flags, 99,
                               protectedVideo.payload.data(),
                               protectedVideo.payload.size(), recovered) ==
            rcsecurity::Error::None && recovered == video,
        "valid video decrypts after rejected tampering");

  const std::vector<uint8_t> stats{'s','t','a','t','s'};
  rcsecurity::ProtectedPayload protectedStats;
  check(server->protectControl(3, 0, 101, stats.data(), stats.size(), protectedStats) ==
            rcsecurity::Error::None &&
            equalsHex<45>(
                protectedStats.payload,
                "00000000000000007374617473cd85f5a5c7461c8ac2c356adb06282b0ba09872b16ba7065570a54e37e8476a5") &&
            client->unprotectControl(3, protectedStats.flags, 101,
                                     protectedStats.payload.data(),
                                     protectedStats.payload.size(), recovered) ==
                rcsecurity::Error::None,
        "statistics stay HMAC-authenticated when encryption is off");
  check(server->protectMedia(3, 0, 102, stats.data(), stats.size(), protectedStats) ==
            rcsecurity::Error::None &&
            equalsHex<29>(
                protectedStats.payload,
                "000000000000000088db9ffc854562ca8f0cac03d434eb2258499047b3") &&
            client->unprotectMedia(3, protectedStats.flags, 102,
                                   protectedStats.payload.data(),
                                   protectedStats.payload.size(), recovered) ==
                rcsecurity::Error::None && recovered == stats,
        "statistics use their own AEAD key/prefix when encryption is on");

  std::shared_ptr<rcsecurity::ISessionProtector> expired;
  check(rcsecurity::createSessionProtector(
            rcsecurity::SessionRole::Client, keys,
            now - rcsecurity::kMaximumSessionLifetimeSeconds, expired) ==
            rcsecurity::Error::None &&
            expired->protectControl(0, 0, 0, control.data(), control.size(),
                                    protectedControl) == rcsecurity::Error::Expired,
        "protector refuses records at the 24-hour reconnect boundary");
  check(rcsecurity::kMaximumSessionRecordsPerChannel == (uint64_t{1} << 32),
        "per-direction/channel record ceiling is exactly 2^32");
}

void testStoredSessionSecurity() {
  std::printf("Stored session authentication\n");
  constexpr std::string_view serviceId = "0123456789abcdef";
  constexpr std::string_view deviceId = "fedcba9876543210";
  const std::filesystem::path directory = temporaryDirectory();
  rcsecurity::PairingStore store(directory, std::string(serviceId));
  rcsecurity::PairingRecord record;
  record.serviceId = serviceId;
  record.deviceId = deviceId;
  for (size_t index = 0; index < record.key.size(); ++index) {
    record.key[index] = static_cast<uint8_t>(0xa0u + index);
  }
  const uint64_t now = unixNow();
  record.createdUnixSeconds = now;
  record.expiresUnixSeconds = std::numeric_limits<uint64_t>::max();
  check(store.save(record) == rcsecurity::Error::None,
        "stored-auth fixture saves its DPAPI record");
  rcsecurity::StoredSessionSecurity sessions(store, std::string(serviceId));
  rcsecurity::AuthenticationChallenge challenge;
  check(sessions.beginAuthentication("192.0.2.40", deviceId, now, challenge) ==
            rcsecurity::Error::None && challenge.expiresUnixSeconds == now + 10,
        "claimed ID only selects a record and yields a ten-second challenge");
  rcsecurity::Nonce32 clientNonce{};
  clientNonce.fill(0x5a);
  rcsecurity::Key32 clientProof{};
  check(rcsecurity::makeAuthenticationProof(record.key, false, serviceId, deviceId,
                                             challenge.serverNonce, clientNonce,
                                             clientProof) == rcsecurity::Error::None,
        "phone creates the nonce-bound proof");
  rcsecurity::AuthenticationResult result;
  check(sessions.finishAuthentication("192.0.2.40", deviceId, clientNonce,
                                      clientProof, now + 1, result) ==
            rcsecurity::Error::None && result.protector != nullptr,
        "only a valid proof returns the server session protector");
  rcsecurity::Key32 expectedServerProof{};
  check(rcsecurity::makeAuthenticationProof(record.key, true, serviceId, deviceId,
                                             challenge.serverNonce, clientNonce,
                                             expectedServerProof) == rcsecurity::Error::None &&
            result.serverProof == expectedServerProof,
        "stored authentication returns the distinct server proof");

  check(sessions.beginAuthentication("192.0.2.40", deviceId, now + 2, challenge) ==
            rcsecurity::Error::None,
        "successful authentication refunded its provisional rate charge");
  check(rcsecurity::makeAuthenticationProof(record.key, false, serviceId, deviceId,
                                             challenge.serverNonce, clientNonce,
                                             clientProof) == rcsecurity::Error::None &&
            sessions.finishAuthentication("192.0.2.40", deviceId, clientNonce,
                                          clientProof, now + 12, result) ==
                rcsecurity::Error::Expired,
        "stored authentication rejects a proof exactly at its ten-second expiry");
  std::error_code ignored;
  std::filesystem::remove_all(directory, ignored);
}

#endif

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  testRatePolicy();
  testPairingStore();
#if RCSECURITY_OPENSSL_AVAILABLE
  check(rcsecurity::available(), "exact OpenSSL build reports available");
  testRfc9382Vector();
  testPairingRolesAndScrypt();
  testAuthenticationAndEnvelopes();
  testStoredSessionSecurity();
#else
  check(!rcsecurity::available(), "missing or wrong OpenSSL version fails closed");
  rcsecurity::Key32 scalar{};
  rcsecurity::Salt16 salt{};
  check(rcsecurity::derivePasswordScalar("123456", salt, scalar) ==
            rcsecurity::Error::Unavailable,
        "fail-closed build exposes no insecure password fallback");
#endif
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
