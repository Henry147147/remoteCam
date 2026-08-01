#include "rcnet/bonjour_service.h"

#include <array>
#include <cstdio>

#include "rcwin/hr.h"

namespace rcnet {
namespace {

constexpr wchar_t kProtocolVersion[] = L"1";

}  // namespace

// The callback can arrive after we have given up waiting, so the shared state outlives
// the BonjourService when it has to. Freeing it while the DNS client still holds the
// pointer would be a use-after-free during shutdown, which is the worst time for one.
struct BonjourService::Registration {
  HANDLE complete = nullptr;
  DWORD status = ERROR_SUCCESS;
  DNS_SERVICE_REGISTER_REQUEST request{};
  DNS_SERVICE_CANCEL cancel{};
  PDNS_SERVICE_INSTANCE instance = nullptr;
  bool callbackPending = false;

  static void WINAPI onComplete(DWORD status, void* context, PDNS_SERVICE_INSTANCE instance) {
    if (instance) ::DnsServiceFreeInstance(instance);
    auto* registration = static_cast<Registration*>(context);
    if (!registration) return;
    registration->status = status;
    ::SetEvent(registration->complete);
  }
};

std::wstring machineServiceId() {
  // Derived from the computer name rather than persisted: it only has to be stable for
  // this machine, and a file or registry value would be one more thing to install,
  // migrate and clean up. A rename changes the id, which is the documented consequence
  // of a rename anyway.
  std::wstring name = computerDisplayName();
  uint64_t hash = 0xcbf29ce484222325ull;
  for (wchar_t c : name) {
    hash ^= static_cast<uint64_t>(c);
    hash *= 0x100000001b3ull;
  }
  wchar_t buffer[17] = {};
  ::swprintf(buffer, 17, L"%016llx", static_cast<unsigned long long>(hash));
  return std::wstring(buffer);
}

std::wstring computerDisplayName() {
  std::array<wchar_t, MAX_COMPUTERNAME_LENGTH + 1> buffer{};
  DWORD size = static_cast<DWORD>(buffer.size());
  if (::GetComputerNameW(buffer.data(), &size) && size > 0) {
    return std::wstring(buffer.data(), size);
  }
  return L"Windows PC";
}

BonjourService::~BonjourService() { stop(); }

HRESULT BonjourService::start(uint16_t port, const std::wstring& displayName,
                              const std::wstring& serviceId, const std::wstring& capabilities,
                              DWORD timeoutMillis) {
  if (advertising_) return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);

  const std::wstring instanceName = displayName + L"._remotecam._tcp.local";
  // The SRV target must be an mDNS name. Windows accepts a bare computer name here and
  // publishes it verbatim, so the record looks healthy from a Windows browser -- which
  // only reads PTR -- while iOS, which has no LLMNR or NetBIOS, cannot resolve it and
  // drops the service. Measured: target 'HENRYDESKTOP' instead of 'HENRYDESKTOP.local'.
  const std::wstring hostName = displayName + L".local";

  std::array<PCWSTR, 4> keys = {L"v", L"name", L"id", L"caps"};
  std::array<PCWSTR, 4> values = {kProtocolVersion, displayName.c_str(), serviceId.c_str(),
                                  capabilities.c_str()};

  PDNS_SERVICE_INSTANCE instance = ::DnsServiceConstructInstance(
      instanceName.c_str(), hostName.c_str(), nullptr, nullptr, port, 0, 0,
      static_cast<DWORD>(keys.size()), keys.data(), values.data());
  if (!instance) {
    RC_ERR(L"DnsServiceConstructInstance failed for %s", instanceName.c_str());
    return E_FAIL;
  }

  auto* registration = new Registration;
  registration->complete = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!registration->complete) {
    const HRESULT hr = rcwin::hrFromLastError();
    ::DnsServiceFreeInstance(instance);
    delete registration;
    return hr;
  }
  registration->instance = instance;
  registration->request.Version = DNS_QUERY_REQUEST_VERSION1;
  registration->request.InterfaceIndex = 0;
  registration->request.pServiceInstance = instance;
  registration->request.pRegisterCompletionCallback = &Registration::onComplete;
  registration->request.pQueryContext = registration;
  registration->request.unicastEnabled = FALSE;  // mDNS on the local link, not unicast DNS

  const DWORD result = ::DnsServiceRegister(&registration->request, &registration->cancel);
  if (result != DNS_REQUEST_PENDING) {
    ::DnsServiceFreeInstance(instance);
    ::CloseHandle(registration->complete);
    delete registration;
    RC_ERR(L"DnsServiceRegister failed with %u", result);
    return HRESULT_FROM_WIN32(result);
  }
  registration->callbackPending = true;
  registration_ = registration;

  if (::WaitForSingleObject(registration->complete, timeoutMillis) != WAIT_OBJECT_0) {
    RC_ERR(L"Bonjour registration did not complete within %u ms", timeoutMillis);
    stop();
    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  }
  registration->callbackPending = false;

  if (registration->status != ERROR_SUCCESS) {
    const HRESULT hr = HRESULT_FROM_WIN32(registration->status);
    RC_ERR(L"Bonjour registration failed: %s", rcwin::hrMessage(hr).c_str());
    stop();
    return hr;
  }

  // Windows owns its own copy from here; ours is no longer needed.
  ::DnsServiceFreeInstance(registration->instance);
  registration->instance = nullptr;
  advertising_ = true;
  RC_LOG(L"advertising _remotecam._tcp on port %u as %s (id %s)", port, displayName.c_str(),
         serviceId.c_str());
  return S_OK;
}

void BonjourService::stop() {
  if (!registration_) {
    advertising_ = false;
    return;
  }

  if (registration_->callbackPending) {
    ::DnsServiceRegisterCancel(&registration_->cancel);
    // The DNS client owns the context until it confirms completion. If the service is
    // unhealthy and never answers, leak the registration rather than free memory it
    // still holds a pointer to -- a bounded leak at shutdown beats a crash.
    if (::WaitForSingleObject(registration_->complete, 5000) != WAIT_OBJECT_0) {
      RC_WARN(L"Bonjour cancel did not complete; leaking the registration deliberately");
      registration_ = nullptr;
      advertising_ = false;
      return;
    }
  }

  if (registration_->instance) ::DnsServiceFreeInstance(registration_->instance);
  if (registration_->complete) ::CloseHandle(registration_->complete);
  delete registration_;
  registration_ = nullptr;
  advertising_ = false;
}

}  // namespace rcnet
