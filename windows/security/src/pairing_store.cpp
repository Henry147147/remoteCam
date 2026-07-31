#include "rcsecurity/security.h"

#include <windows.h>
#include <dpapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <system_error>

namespace rcsecurity {
namespace {

constexpr std::array<uint8_t, 8> kPlainMagic = {'R', 'C', 'P', 'A', 'I', 'R', '0', '1'};
constexpr std::array<uint8_t, 8> kFileMagic = {'R', 'C', 'D', 'P', 'A', 'P', 'I', '1'};
constexpr size_t kPlainBytes = 8 + 16 + 16 + 8 + 8 + 32;
constexpr DWORD kMaxFileBytes = 64u * 1024u;
std::atomic<uint64_t> g_tempCounter{0};

bool validId(std::string_view id) {
  if (id.size() != 16) return false;
  for (char c : id) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  }
  return true;
}

void appendU32(Bytes& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value >> 24));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}

void appendU64(Bytes& out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>(value >> shift));
  }
}

uint32_t readU32(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24) |
         (static_cast<uint32_t>(data[1]) << 16) |
         (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

uint64_t readU64(const uint8_t* data) {
  uint64_t value = 0;
  for (size_t index = 0; index < 8; ++index) value = (value << 8) | data[index];
  return value;
}

Bytes entropyFor(std::string_view serviceId) {
  constexpr std::string_view domain = "RemoteCam DPAPI pairing record v1";
  Bytes entropy(domain.begin(), domain.end());
  entropy.insert(entropy.end(), serviceId.begin(), serviceId.end());
  return entropy;
}

Error protect(const Bytes& plaintext, std::string_view serviceId, Bytes& protectedBytes) {
  Bytes entropy = entropyFor(serviceId);
  DATA_BLOB input{static_cast<DWORD>(plaintext.size()),
                  const_cast<BYTE*>(reinterpret_cast<const BYTE*>(plaintext.data()))};
  DATA_BLOB optionalEntropy{static_cast<DWORD>(entropy.size()), entropy.data()};
  DATA_BLOB output{};
  if (!::CryptProtectData(&input, L"RemoteCam pairing key", &optionalEntropy, nullptr,
                          nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
    return Error::StorageFailure;
  }
  protectedBytes.assign(output.pbData, output.pbData + output.cbData);
  ::SecureZeroMemory(output.pbData, output.cbData);
  ::LocalFree(output.pbData);
  return Error::None;
}

Error unprotect(const uint8_t* data, size_t size, std::string_view serviceId,
                Bytes& plaintext) {
  if (size == 0 || size > MAXDWORD) return Error::CorruptRecord;
  Bytes entropy = entropyFor(serviceId);
  DATA_BLOB input{static_cast<DWORD>(size),
                  const_cast<BYTE*>(reinterpret_cast<const BYTE*>(data))};
  DATA_BLOB optionalEntropy{static_cast<DWORD>(entropy.size()), entropy.data()};
  DATA_BLOB output{};
  if (!::CryptUnprotectData(&input, nullptr, &optionalEntropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output)) {
    return Error::CorruptRecord;
  }
  plaintext.assign(output.pbData, output.pbData + output.cbData);
  ::SecureZeroMemory(output.pbData, output.cbData);
  ::LocalFree(output.pbData);
  return Error::None;
}

Error writeAll(HANDLE file, const Bytes& bytes) {
  size_t offset = 0;
  while (offset < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(
        (bytes.size() - offset) > MAXDWORD ? MAXDWORD : bytes.size() - offset);
    DWORD written = 0;
    if (!::WriteFile(file, bytes.data() + offset, chunk, &written, nullptr) || written == 0) {
      return Error::StorageFailure;
    }
    offset += written;
  }
  return ::FlushFileBuffers(file) ? Error::None : Error::StorageFailure;
}

Error readFile(const std::filesystem::path& path, Bytes& bytes) {
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return ::GetLastError() == ERROR_FILE_NOT_FOUND ? Error::NotFound : Error::StorageFailure;
  }
  LARGE_INTEGER size{};
  if (!::GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
      size.QuadPart > kMaxFileBytes) {
    ::CloseHandle(file);
    return Error::CorruptRecord;
  }
  bytes.resize(static_cast<size_t>(size.QuadPart));
  size_t offset = 0;
  while (offset < bytes.size()) {
    DWORD read = 0;
    const DWORD chunk = static_cast<DWORD>(bytes.size() - offset);
    if (!::ReadFile(file, bytes.data() + offset, chunk, &read, nullptr) || read == 0) {
      ::CloseHandle(file);
      return Error::StorageFailure;
    }
    offset += read;
  }
  ::CloseHandle(file);
  return Error::None;
}

}  // namespace

PairingStore::PairingStore(std::filesystem::path directory, std::string serviceId)
    : directory_(std::move(directory)), serviceId_(std::move(serviceId)) {}

std::filesystem::path PairingStore::defaultDirectory() {
  const DWORD required = ::GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
  if (required == 0) return {};
  std::wstring value(required, L'\0');
  const DWORD copied = ::GetEnvironmentVariableW(L"LOCALAPPDATA", value.data(), required);
  if (copied == 0 || copied >= required) return {};
  value.resize(copied);
  return std::filesystem::path(value) / L"RemoteCam" / L"pairings";
}

Error PairingStore::save(const PairingRecord& record) const {
  if (!validId(serviceId_) || record.serviceId != serviceId_ ||
      !validId(record.deviceId) || record.expiresUnixSeconds <= record.createdUnixSeconds ||
      directory_.empty()) {
    return Error::InvalidArgument;
  }

  Bytes plaintext;
  plaintext.reserve(kPlainBytes);
  plaintext.insert(plaintext.end(), kPlainMagic.begin(), kPlainMagic.end());
  plaintext.insert(plaintext.end(), record.serviceId.begin(), record.serviceId.end());
  plaintext.insert(plaintext.end(), record.deviceId.begin(), record.deviceId.end());
  appendU64(plaintext, record.createdUnixSeconds);
  appendU64(plaintext, record.expiresUnixSeconds);
  plaintext.insert(plaintext.end(), record.key.begin(), record.key.end());

  Bytes encrypted;
  const Error protectError = protect(plaintext, serviceId_, encrypted);
  ::SecureZeroMemory(plaintext.data(), plaintext.size());
  if (protectError != Error::None) return protectError;

  Bytes fileBytes;
  fileBytes.reserve(kFileMagic.size() + 4 + encrypted.size());
  fileBytes.insert(fileBytes.end(), kFileMagic.begin(), kFileMagic.end());
  appendU32(fileBytes, static_cast<uint32_t>(encrypted.size()));
  fileBytes.insert(fileBytes.end(), encrypted.begin(), encrypted.end());

  std::error_code filesystemError;
  std::filesystem::create_directories(directory_, filesystemError);
  if (filesystemError) return Error::StorageFailure;

  const std::filesystem::path target = directory_ / (record.deviceId + ".rcpair");
  const uint64_t counter = g_tempCounter.fetch_add(1, std::memory_order_relaxed);
  const std::filesystem::path temporary =
      target.wstring() + L".tmp." + std::to_wstring(::GetCurrentProcessId()) + L"." +
      std::to_wstring(counter);
  HANDLE file = ::CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY, nullptr);
  if (file == INVALID_HANDLE_VALUE) return Error::StorageFailure;
  const Error writeError = writeAll(file, fileBytes);
  ::CloseHandle(file);
  if (writeError != Error::None ||
      !::MoveFileExW(temporary.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    ::DeleteFileW(temporary.c_str());
    return Error::StorageFailure;
  }
  return Error::None;
}

Error PairingStore::load(std::string_view deviceId, PairingRecord& record) const {
  if (!validId(serviceId_) || !validId(deviceId) || directory_.empty()) {
    return Error::InvalidArgument;
  }
  Bytes fileBytes;
  const Error readError = readFile(directory_ / (std::string(deviceId) + ".rcpair"), fileBytes);
  if (readError != Error::None) return readError;
  if (fileBytes.size() < kFileMagic.size() + 4 ||
      !std::equal(kFileMagic.begin(), kFileMagic.end(), fileBytes.begin())) {
    return Error::CorruptRecord;
  }
  const uint32_t encryptedSize = readU32(fileBytes.data() + kFileMagic.size());
  if (encryptedSize != fileBytes.size() - kFileMagic.size() - 4) {
    return Error::CorruptRecord;
  }

  Bytes plaintext;
  const Error unprotectError =
      unprotect(fileBytes.data() + kFileMagic.size() + 4, encryptedSize, serviceId_, plaintext);
  if (unprotectError != Error::None) return unprotectError;
  if (plaintext.size() != kPlainBytes ||
      !std::equal(kPlainMagic.begin(), kPlainMagic.end(), plaintext.begin())) {
    ::SecureZeroMemory(plaintext.data(), plaintext.size());
    return Error::CorruptRecord;
  }

  size_t offset = kPlainMagic.size();
  PairingRecord parsed;
  parsed.serviceId.assign(reinterpret_cast<const char*>(plaintext.data() + offset), 16);
  offset += 16;
  parsed.deviceId.assign(reinterpret_cast<const char*>(plaintext.data() + offset), 16);
  offset += 16;
  parsed.createdUnixSeconds = readU64(plaintext.data() + offset);
  offset += 8;
  parsed.expiresUnixSeconds = readU64(plaintext.data() + offset);
  offset += 8;
  std::copy_n(plaintext.data() + offset, parsed.key.size(), parsed.key.begin());
  ::SecureZeroMemory(plaintext.data(), plaintext.size());

  if (parsed.serviceId != serviceId_ || parsed.deviceId != deviceId ||
      parsed.expiresUnixSeconds <= parsed.createdUnixSeconds) {
    ::SecureZeroMemory(parsed.key.data(), parsed.key.size());
    return Error::CorruptRecord;
  }
  record = parsed;
  ::SecureZeroMemory(parsed.key.data(), parsed.key.size());
  return Error::None;
}

Error PairingStore::erase(std::string_view deviceId) const {
  if (!validId(deviceId) || directory_.empty()) return Error::InvalidArgument;
  const std::filesystem::path path = directory_ / (std::string(deviceId) + ".rcpair");
  if (::DeleteFileW(path.c_str())) return Error::None;
  return ::GetLastError() == ERROR_FILE_NOT_FOUND ? Error::None : Error::StorageFailure;
}

}  // namespace rcsecurity
