// rc-vcam-register.exe -- elevated install-time camera and firewall operations.
//
// Two separate things have to happen before Windows will load rc-vcam.dll:
//
//   1. The CLSID must resolve to the DLL, under HKLM\SOFTWARE\Classes\CLSID\{...}.
//   2. MFCreateVirtualCamera must be told that CLSID represents a camera.
//
// Camera registration and the machine firewall policy need administrator rights once,
// at install time -- never at app launch. The running application must never elevate.

#include <windows.h>

#include <aclapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>
#include <netfw.h>
#include <restartmanager.h>
#include <sddl.h>
#include <shlobj.h>
#include <wrl/client.h>

#include <cstdio>
#include <string>
#include <vector>

#include "rcwin/guids.h"
#include "rcwin/hr.h"

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kFirewallRuleName[] = L"RemoteCam";
constexpr wchar_t kFirewallRuleDescription[] =
    L"Allows paired phones to stream video to RemoteCam on private networks.";
constexpr wchar_t kFirewallLocalPort[] = L"7890";

class ScopedBstr final {
 public:
  ScopedBstr() = default;
  explicit ScopedBstr(const wchar_t* value) : value_(::SysAllocString(value)) {}
  ~ScopedBstr() { ::SysFreeString(value_); }

  ScopedBstr(const ScopedBstr&) = delete;
  ScopedBstr& operator=(const ScopedBstr&) = delete;

  [[nodiscard]] BSTR get() const { return value_; }
  [[nodiscard]] BSTR* put() {
    ::SysFreeString(value_);
    value_ = nullptr;
    return &value_;
  }
  [[nodiscard]] bool valid() const { return value_ != nullptr; }

 private:
  BSTR value_ = nullptr;
};

void print(const wchar_t* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  std::vwprintf(fmt, args);
  va_end(args);
}

bool isElevated() {
  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  TOKEN_ELEVATION elevation{};
  DWORD size = sizeof(elevation);
  const bool ok =
      ::GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != 0;
  ::CloseHandle(token);
  return ok && elevation.TokenIsElevated != 0;
}

std::wstring directoryOf(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring defaultDllPath() {
  const std::wstring dir = directoryOf(rcwin::modulePath());
  return dir.empty() ? std::wstring(L"rc-vcam.dll") : dir + L"\\rc-vcam.dll";
}

HRESULT normalizedExistingFile(const std::wstring& path, std::wstring& out) {
  if (path.empty()) return E_INVALIDARG;

  const DWORD required = ::GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
  if (required == 0) return RC_HR_FROM_LAST_ERROR();
  std::vector<wchar_t> buffer(static_cast<size_t>(required));
  const DWORD written =
      ::GetFullPathNameW(path.c_str(), required, buffer.data(), nullptr);
  if (written == 0) return RC_HR_FROM_LAST_ERROR();
  if (written >= required) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);

  const DWORD attributes = ::GetFileAttributesW(buffer.data());
  if (attributes == INVALID_FILE_ATTRIBUTES) return RC_HR_FROM_LAST_ERROR();
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
  }
  out.assign(buffer.data(), written);
  return S_OK;
}

HRESULT verifyFileCanBeDeleted(const std::wstring& path) {
  const DWORD attributes = ::GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD error = ::GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
               ? S_OK
               : HRESULT_FROM_WIN32(error);
  }
  if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY)) != 0) {
    return HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED);
  }

  const HANDLE file =
      ::CreateFileW(path.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return RC_HR_FROM_LAST_ERROR();
  ::CloseHandle(file);
  return S_OK;
}

HRESULT verifyFilesAreUnused(const std::vector<std::wstring>& paths) {
  if (paths.empty()) return S_OK;

  DWORD session = 0;
  WCHAR sessionKey[CCH_RM_SESSION_KEY + 1]{};
  DWORD status = ::RmStartSession(&session, 0, sessionKey);
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);

  std::vector<LPCWSTR> resources;
  resources.reserve(paths.size());
  for (const std::wstring& path : paths) resources.push_back(path.c_str());

  status = ::RmRegisterResources(session, static_cast<UINT>(resources.size()),
                                 resources.data(), 0, nullptr, 0, nullptr);
  HRESULT result = status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status);

  std::vector<RM_PROCESS_INFO> processes;
  if (SUCCEEDED(result)) {
    UINT needed = 0;
    UINT count = 0;
    DWORD rebootReasons = RmRebootReasonNone;
    status = ::RmGetList(session, &needed, &count, nullptr, &rebootReasons);
    while (status == ERROR_MORE_DATA && needed != 0) {
      processes.resize(needed);
      count = static_cast<UINT>(processes.size());
      status = ::RmGetList(session, &needed, &count, processes.data(), &rebootReasons);
    }
    if (status != ERROR_SUCCESS) {
      result = HRESULT_FROM_WIN32(status);
    } else if (count != 0) {
      for (UINT i = 0; i < count; ++i) {
        print(L"  file in use   : %s (PID %lu)\n", processes[i].strAppName,
              processes[i].Process.dwProcessId);
      }
      result = HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION);
    } else if (rebootReasons != RmRebootReasonNone) {
      print(L"  delete check  : Restart Manager requires a reboot (reasons 0x%08lx)\n",
            rebootReasons);
      result = HRESULT_FROM_WIN32(ERROR_SUCCESS_REBOOT_REQUIRED);
    }
  }

  const DWORD endStatus = ::RmEndSession(session);
  if (SUCCEEDED(result) && endStatus != ERROR_SUCCESS) {
    result = HRESULT_FROM_WIN32(endStatus);
  }
  return result;
}

HRESULT uninstallPayloadPreflight() {
  const std::wstring directory = directoryOf(rcwin::modulePath());
  if (directory.empty()) return HRESULT_FROM_WIN32(ERROR_BAD_PATHNAME);

  std::vector<std::wstring> paths;
  for (const wchar_t* filename : {L"RemoteCam.exe", L"rc-vcam.dll"}) {
    paths.push_back(directory + L"\\" + filename);
  }

  std::vector<std::wstring> existingPaths;
  for (const std::wstring& path : paths) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
      existingPaths.push_back(path);
    } else {
      const DWORD error = ::GetLastError();
      if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
        return HRESULT_FROM_WIN32(error);
      }
    }
  }

  RC_RETURN_IF_FAILED(verifyFilesAreUnused(existingPaths));
  for (const std::wstring& path : paths) {
    const HRESULT hr = verifyFileCanBeDeleted(path);
    print(L"  delete check  : %s -- %s\n", path.c_str(),
          SUCCEEDED(hr) ? L"available" : rcwin::hrMessage(hr).c_str());
    if (FAILED(hr)) return hr;
  }
  return S_OK;
}

HRESULT firewallPolicy(ComPtr<INetFwPolicy2>& policy, ComPtr<INetFwRules>& rules) {
  RC_RETURN_IF_FAILED(::CoCreateInstance(__uuidof(NetFwPolicy2), nullptr,
                                         CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(policy.ReleaseAndGetAddressOf())));
  return policy->get_Rules(rules.ReleaseAndGetAddressOf());
}

HRESULT privatePolicyEffectiveness(INetFwPolicy2* policy) {
  long currentProfiles = 0;
  RC_RETURN_IF_FAILED(policy->get_CurrentProfileTypes(&currentProfiles));
  if ((currentProfiles & NET_FW_PROFILE2_PRIVATE) == 0) return S_FALSE;

  NET_FW_MODIFY_STATE state = NET_FW_MODIFY_STATE_OK;
  RC_RETURN_IF_FAILED(policy->get_LocalPolicyModifyState(&state));
  switch (state) {
    case NET_FW_MODIFY_STATE_OK:
      return S_OK;
    case NET_FW_MODIFY_STATE_GP_OVERRIDE:
      return HRESULT_FROM_WIN32(ERROR_ACCESS_DISABLED_BY_POLICY);
    case NET_FW_MODIFY_STATE_INBOUND_BLOCKED:
      return HRESULT_FROM_WIN32(ERROR_NETWORK_ACCESS_DENIED);
    default:
      return E_UNEXPECTED;
  }
}

HRESULT verifyFirewallRule(INetFwRules* rules, const std::wstring& appPath) {
  ScopedBstr name(kFirewallRuleName);
  if (!name.valid()) return E_OUTOFMEMORY;

  ComPtr<INetFwRule> rule;
  RC_RETURN_IF_FAILED(rules->Item(name.get(), rule.GetAddressOf()));

  ScopedBstr actualApp;
  ScopedBstr actualPorts;
  long protocol = 0;
  NET_FW_RULE_DIRECTION direction = NET_FW_RULE_DIR_MAX;
  VARIANT_BOOL enabled = VARIANT_FALSE;
  long profiles = 0;
  NET_FW_ACTION action = NET_FW_ACTION_MAX;
  RC_RETURN_IF_FAILED(rule->get_ApplicationName(actualApp.put()));
  RC_RETURN_IF_FAILED(rule->get_Protocol(&protocol));
  RC_RETURN_IF_FAILED(rule->get_LocalPorts(actualPorts.put()));
  RC_RETURN_IF_FAILED(rule->get_Direction(&direction));
  RC_RETURN_IF_FAILED(rule->get_Enabled(&enabled));
  RC_RETURN_IF_FAILED(rule->get_Profiles(&profiles));
  RC_RETURN_IF_FAILED(rule->get_Action(&action));

  const bool appMatches =
      actualApp.get() != nullptr &&
      ::CompareStringOrdinal(actualApp.get(), -1, appPath.c_str(), -1, TRUE) == CSTR_EQUAL;
  const bool portsMatch =
      actualPorts.get() != nullptr && wcscmp(actualPorts.get(), kFirewallLocalPort) == 0;
  if (!appMatches || protocol != NET_FW_IP_PROTOCOL_TCP || !portsMatch ||
      direction != NET_FW_RULE_DIR_IN || enabled != VARIANT_TRUE ||
      profiles != NET_FW_PROFILE2_PRIVATE || action != NET_FW_ACTION_ALLOW) {
    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
  }
  return S_OK;
}

HRESULT captureExistingFirewallRule(INetFwRules* rules, bool& exists,
                                    std::wstring& appPath) {
  exists = false;
  appPath.clear();

  ScopedBstr name(kFirewallRuleName);
  if (!name.valid()) return E_OUTOFMEMORY;
  ComPtr<INetFwRule> existing;
  const HRESULT itemHr = rules->Item(name.get(), existing.GetAddressOf());
  if (itemHr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return S_OK;
  RC_RETURN_IF_FAILED(itemHr);

  ScopedBstr app;
  RC_RETURN_IF_FAILED(existing->get_ApplicationName(app.put()));
  if (app.get() == nullptr || app.get()[0] == L'\0') {
    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
  }
  appPath.assign(app.get());

  // Refuse to overwrite an unrelated rule that merely happens to use our name.
  RC_RETURN_IF_FAILED(verifyFirewallRule(rules, appPath));
  exists = true;
  return S_OK;
}

HRESULT createFirewallRule(const std::wstring& appPath, ComPtr<INetFwRule>& rule) {
  RC_RETURN_IF_FAILED(::CoCreateInstance(__uuidof(NetFwRule), nullptr,
                                         CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(rule.ReleaseAndGetAddressOf())));

  ScopedBstr name(kFirewallRuleName);
  ScopedBstr description(kFirewallRuleDescription);
  ScopedBstr app(appPath.c_str());
  ScopedBstr port(kFirewallLocalPort);
  if (!name.valid() || !description.valid() || !app.valid() || !port.valid()) {
    return E_OUTOFMEMORY;
  }

  RC_RETURN_IF_FAILED(rule->put_Name(name.get()));
  RC_RETURN_IF_FAILED(rule->put_Description(description.get()));
  RC_RETURN_IF_FAILED(rule->put_ApplicationName(app.get()));
  RC_RETURN_IF_FAILED(rule->put_Protocol(NET_FW_IP_PROTOCOL_TCP));
  RC_RETURN_IF_FAILED(rule->put_LocalPorts(port.get()));
  RC_RETURN_IF_FAILED(rule->put_Direction(NET_FW_RULE_DIR_IN));
  RC_RETURN_IF_FAILED(rule->put_Profiles(NET_FW_PROFILE2_PRIVATE));
  RC_RETURN_IF_FAILED(rule->put_Action(NET_FW_ACTION_ALLOW));
  return rule->put_Enabled(VARIANT_TRUE);
}

HRESULT removeFirewallRule(INetFwRules* rules) {
  ScopedBstr name(kFirewallRuleName);
  if (!name.valid()) return E_OUTOFMEMORY;

  ComPtr<INetFwRule> existing;
  HRESULT hr = rules->Item(name.get(), existing.GetAddressOf());
  if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return S_FALSE;
  RC_RETURN_IF_FAILED(hr);
  RC_RETURN_IF_FAILED(rules->Remove(name.get()));

  existing.Reset();
  hr = rules->Item(name.get(), existing.GetAddressOf());
  if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return S_OK;
  if (SUCCEEDED(hr)) return HRESULT_FROM_WIN32(ERROR_BUSY);
  return hr;
}

HRESULT addFirewallRule(const std::wstring& requestedAppPath) {
  std::wstring appPath;
  RC_RETURN_IF_FAILED(normalizedExistingFile(requestedAppPath, appPath));

  ComPtr<INetFwPolicy2> policy;
  ComPtr<INetFwRules> rules;
  RC_RETURN_IF_FAILED(firewallPolicy(policy, rules));
  const HRESULT policyHr = privatePolicyEffectiveness(policy.Get());
  if (FAILED(policyHr)) return policyHr;

  bool previousExists = false;
  std::wstring previousAppPath;
  RC_RETURN_IF_FAILED(
      captureExistingFirewallRule(rules.Get(), previousExists, previousAppPath));

  ComPtr<INetFwRule> rule;
  RC_RETURN_IF_FAILED(createFirewallRule(appPath, rule));

  // Add is the commit point: Windows overwrites a rule with the same identifier, so
  // construction/property failures above leave the working rule untouched.
  RC_RETURN_IF_FAILED(rules->Add(rule.Get()));

  const HRESULT verifyHr = verifyFirewallRule(rules.Get(), appPath);
  if (FAILED(verifyHr)) {
    HRESULT rollbackHr = S_OK;
    if (previousExists) {
      ComPtr<INetFwRule> previousRule;
      rollbackHr = createFirewallRule(previousAppPath, previousRule);
      if (SUCCEEDED(rollbackHr)) rollbackHr = rules->Add(previousRule.Get());
      if (SUCCEEDED(rollbackHr)) {
        rollbackHr = verifyFirewallRule(rules.Get(), previousAppPath);
      }
    } else {
      rollbackHr = removeFirewallRule(rules.Get());
    }
    return FAILED(rollbackHr) ? rollbackHr : verifyHr;
  }
  return policyHr;
}

HRESULT removeFirewallRule() {
  ComPtr<INetFwPolicy2> policy;
  ComPtr<INetFwRules> rules;
  RC_RETURN_IF_FAILED(firewallPolicy(policy, rules));
  return removeFirewallRule(rules.Get());
}

// The Frame Server runs as LOCAL SERVICE and cannot write anywhere useful by default.
// Without this directory rc-vcam.dll's log silently goes nowhere, and a Session 0
// failure becomes undiagnosable -- which is the single worst place to be blind.
HRESULT ensureLogDirectory() {
  PWSTR base = nullptr;
  RC_RETURN_IF_FAILED(::SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &base));
  const std::wstring root = std::wstring(base) + L"\\RemoteCam";
  ::CoTaskMemFree(base);
  const std::wstring logs = root + L"\\logs";

  // OICI: inherited by files and subdirectories, so the log file itself is writable by
  // LOCAL SERVICE without a second ACL edit.
  static constexpr wchar_t kSddl[] =
      L"D:(A;OICI;GA;;;SY)(A;OICI;GA;;;BA)(A;OICI;GRGWGX;;;LS)(A;OICI;GRGWGX;;;IU)";

  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(kSddl, SDDL_REVISION_1, &sd,
                                                              nullptr)) {
    return RC_HR_FROM_LAST_ERROR();
  }
  SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};

  HRESULT hr = S_OK;
  for (const std::wstring& dir : {root, logs}) {
    if (!::CreateDirectoryW(dir.c_str(), &sa)) {
      const DWORD err = ::GetLastError();
      if (err == ERROR_ALREADY_EXISTS) {
        // Existing directory: apply the DACL explicitly, since the SECURITY_ATTRIBUTES
        // above are only honoured at creation time.
        BOOL present = FALSE, defaulted = FALSE;
        PACL dacl = nullptr;
        if (::GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) && present) {
          ::SetNamedSecurityInfoW(const_cast<PWSTR>(dir.c_str()), SE_FILE_OBJECT,
                                  DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
        }
      } else {
        hr = HRESULT_FROM_WIN32(err);
        break;
      }
    }
  }
  ::LocalFree(sd);
  if (SUCCEEDED(hr)) print(L"  log directory : %s\n", logs.c_str());
  return hr;
}

HRESULT writeRegistry(const std::wstring& dllPath) {
  HKEY key = nullptr;
  // HKLM\SOFTWARE\Classes, never HKCR. Under UAC, HKCR writes are redirected into a
  // per-user hive; Session 0 cannot read it, and the resulting camera enumerates
  // normally and then never produces a frame. That failure looks like a media source
  // bug and costs hours before anyone suspects the registry.
  LONG status = ::RegCreateKeyExW(HKEY_LOCAL_MACHINE, rcwin::kClsidKeyPath, 0, nullptr,
                                  REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key, nullptr);
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);

  status = ::RegSetValueExW(
      key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(rcwin::kComDescription),
      static_cast<DWORD>((wcslen(rcwin::kComDescription) + 1) * sizeof(wchar_t)));
  ::RegCloseKey(key);
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);

  status = ::RegCreateKeyExW(HKEY_LOCAL_MACHINE, rcwin::kInprocKeyPath, 0, nullptr,
                             REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &key, nullptr);
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);

  status = ::RegSetValueExW(key, nullptr, 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(dllPath.c_str()),
                            static_cast<DWORD>((dllPath.size() + 1) * sizeof(wchar_t)));
  if (status == ERROR_SUCCESS) {
    // "Both" lets the Frame Server activate us on whichever apartment it pleases. A
    // media source is internally free-threaded, so constraining it to Apartment would
    // add a marshalling hop to every single frame.
    static constexpr wchar_t kBoth[] = L"Both";
    status = ::RegSetValueExW(key, L"ThreadingModel", 0, REG_SZ,
                              reinterpret_cast<const BYTE*>(kBoth), sizeof(kBoth));
  }
  ::RegCloseKey(key);
  return status == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(status);
}

HRESULT removeRegistry() {
  LONG a = ::RegDeleteKeyExW(HKEY_LOCAL_MACHINE, rcwin::kInprocKeyPath, KEY_WOW64_64KEY, 0);
  LONG b = ::RegDeleteKeyExW(HKEY_LOCAL_MACHINE, rcwin::kClsidKeyPath, KEY_WOW64_64KEY, 0);
  if (a == ERROR_FILE_NOT_FOUND) a = ERROR_SUCCESS;
  if (b == ERROR_FILE_NOT_FOUND) b = ERROR_SUCCESS;
  if (a != ERROR_SUCCESS) return HRESULT_FROM_WIN32(a);
  return b == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(b);
}

HRESULT readRegisteredPath(std::wstring& out) {
  DWORD bytes = 0;
  LONG status = ::RegGetValueW(HKEY_LOCAL_MACHINE, rcwin::kInprocKeyPath, nullptr,
                               RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
  if (bytes < sizeof(wchar_t) || bytes % sizeof(wchar_t) != 0) {
    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
  }
  std::vector<wchar_t> buffer(bytes / sizeof(wchar_t));
  status = ::RegGetValueW(HKEY_LOCAL_MACHINE, rcwin::kInprocKeyPath, nullptr,
                          RRF_RT_REG_SZ, nullptr, buffer.data(), &bytes);
  if (status != ERROR_SUCCESS) return HRESULT_FROM_WIN32(status);
  if (buffer.empty() || buffer.back() != L'\0') {
    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
  }
  out.assign(buffer.data());
  return S_OK;
}

HRESULT createCamera(MFVirtualCameraLifetime lifetime, IMFVirtualCamera** out) {
  return ::MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, lifetime,
                                 MFVirtualCameraAccess_AllUsers, rcwin::kFriendlyName,
                                 rcwin::kSourceClsidString, nullptr, 0, out);
}

// Enumerates video capture devices through Media Foundation and reports whether ours
// is among them. This is the difference between "we wrote some registry keys" and
// "Windows agrees a camera exists", which are very different claims.
HRESULT reportEnumeration(const wchar_t* expectedSymbolicLink = nullptr) {
  ComPtr<IMFAttributes> attributes;
  RC_RETURN_IF_FAILED(::MFCreateAttributes(&attributes, 1));
  RC_RETURN_IF_FAILED(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                          MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));

  IMFActivate** devices = nullptr;
  UINT32 count = 0;
  RC_RETURN_IF_FAILED(::MFEnumDeviceSources(attributes.Get(), &devices, &count));

  bool found = false;
  HRESULT symbolicLinkHr = S_OK;
  print(L"  cameras (MF): %u\n", count);
  for (UINT32 i = 0; i < count; ++i) {
    WCHAR* name = nullptr;
    WCHAR* symbolicLink = nullptr;
    UINT32 length = 0;
    const HRESULT nameHr = devices[i]->GetAllocatedString(
        MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &length);
    if (expectedSymbolicLink != nullptr) {
      UINT32 symbolicLinkLength = 0;
      HRESULT linkHr = devices[i]->GetAllocatedString(
          MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &symbolicLink,
          &symbolicLinkLength);
      if (SUCCEEDED(linkHr) && (symbolicLink == nullptr || symbolicLinkLength == 0)) {
        linkHr = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
      }
      if (FAILED(linkHr) && SUCCEEDED(symbolicLinkHr)) symbolicLinkHr = linkHr;
    }

    const bool nameMatches = SUCCEEDED(nameHr) && name != nullptr &&
                             wcscmp(name, rcwin::kFriendlyName) == 0;
    const bool linkMatches = expectedSymbolicLink != nullptr && symbolicLink != nullptr &&
                             _wcsicmp(symbolicLink, expectedSymbolicLink) == 0;
    const bool ours = expectedSymbolicLink != nullptr ? linkMatches : nameMatches;
    found = found || ours;
    if (name != nullptr) {
      print(L"    %s%s\n", name, ours ? L"   <-- RemoteCam" : L"");
    } else if (ours) {
      print(L"    (unnamed camera)   <-- RemoteCam\n");
    }
    ::CoTaskMemFree(name);
    ::CoTaskMemFree(symbolicLink);
    devices[i]->Release();
  }
  ::CoTaskMemFree(devices);

  // An exact absence claim is unsafe if any enumerated device withheld its symbolic
  // link: that unreadable entry could be the camera we are trying to remove.
  if (!found && expectedSymbolicLink != nullptr && FAILED(symbolicLinkHr)) {
    return symbolicLinkHr;
  }
  print(L"  RemoteCam enumerates: %s\n", found ? L"YES" : L"NO");
  return found ? S_OK : S_FALSE;
}

HRESULT confirmCameraAbsent(const wchar_t* expectedSymbolicLink) {
  if (expectedSymbolicLink == nullptr || expectedSymbolicLink[0] == L'\0') {
    return E_INVALIDARG;
  }

  // Device-interface removal can take a moment to become visible to a fresh MF
  // enumeration. Never remove the CLSID until the exact symbolic link is gone.
  constexpr int kAttempts = 10;
  for (int attempt = 0; attempt < kAttempts; ++attempt) {
    const HRESULT enumerationHr = reportEnumeration(expectedSymbolicLink);
    if (enumerationHr == S_FALSE) return S_OK;
    if (FAILED(enumerationHr)) return enumerationHr;
    if (attempt + 1 < kAttempts) ::Sleep(100);
  }
  return HRESULT_FROM_WIN32(ERROR_BUSY);
}

HRESULT removeCameraAndConfirm(IMFVirtualCamera* camera,
                               const wchar_t* knownSymbolicLink = nullptr) {
  if (camera == nullptr) return E_POINTER;
  if (knownSymbolicLink != nullptr && knownSymbolicLink[0] == L'\0') return E_INVALIDARG;

  WCHAR* allocatedSymbolicLink = nullptr;
  const wchar_t* symbolicLink = knownSymbolicLink;
  if (symbolicLink == nullptr) {
    UINT32 symbolicLinkLength = 0;
    const HRESULT linkHr = camera->GetAllocatedString(
        MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &allocatedSymbolicLink,
        &symbolicLinkLength);
    if (linkHr == MF_E_ATTRIBUTENOTFOUND) {
      ::CoTaskMemFree(allocatedSymbolicLink);
      print(L"  camera        : already absent (no registered symbolic link)\n");
      return S_FALSE;
    }
    if (FAILED(linkHr)) {
      ::CoTaskMemFree(allocatedSymbolicLink);
      return linkHr;
    }
    if (allocatedSymbolicLink == nullptr || symbolicLinkLength == 0) {
      ::CoTaskMemFree(allocatedSymbolicLink);
      return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    }
    symbolicLink = allocatedSymbolicLink;
  }

  const HRESULT removeHr = camera->Remove();
  const HRESULT absenceHr = confirmCameraAbsent(symbolicLink);
  ::CoTaskMemFree(allocatedSymbolicLink);

  // Exact absence is authoritative even if Remove returned a stale/transient error.
  if (SUCCEEDED(absenceHr)) return S_OK;
  return FAILED(removeHr) ? removeHr : absenceHr;
}

int usage() {
  print(
      L"rc-vcam-register -- RemoteCam virtual camera registration\n"
      L"\n"
      L"  --status                 report registration state (no elevation needed)\n"
      L"  --register [--dll PATH]  register the CLSID and create the camera  [admin]\n"
      L"  --unregister             remove the camera and the CLSID           [admin]\n"
      L"  --firewall-add --app PATH\n"
      L"                           replace and verify the private TCP 7890 rule [admin]\n"
      L"  --firewall-remove        remove the RemoteCam firewall rule          [admin]\n"
      L"  --uninstall-preflight    verify the app and camera DLL are removable [admin]\n"
      L"  --session                dev mode: create a Session-lifetime camera and hold\n"
      L"                           it until Ctrl+C. Needs the CLSID already registered,\n"
      L"                           and administrator rights for AllUsers access.\n"
      L"\n");
  return 2;
}

int requireElevation(const wchar_t* verb) {
  print(L"ERROR: --%s needs administrator rights.\n\n", verb);
  print(L"Run this from an elevated prompt:\n\n    \"%s\" --%s\n\n",
        rcwin::modulePath().c_str(), verb);
  return 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  rcwin::logInit(L"rc-vcam-register");

  std::wstring mode;
  std::wstring dllPath = defaultDllPath();
  std::wstring appPath;
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    if (arg == L"--dll" && i + 1 < argc) {
      dllPath = argv[++i];
    } else if (arg == L"--app" && i + 1 < argc) {
      appPath = argv[++i];
    } else if (arg == L"--status" || arg == L"--register" ||
               arg == L"--unregister" || arg == L"--firewall-add" ||
               arg == L"--firewall-remove" || arg == L"--uninstall-preflight" ||
               arg == L"--session") {
      if (!mode.empty()) return usage();
      mode = arg.substr(2);
    } else {
      return usage();
    }
  }
  if (mode.empty()) return usage();

  const HRESULT comHr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(comHr)) {
    print(L"CoInitializeEx failed: %s\n", rcwin::hrMessage(comHr).c_str());
    return 1;
  }
  if (mode == L"firewall-add" || mode == L"firewall-remove" ||
      mode == L"uninstall-preflight") {
    int utilityExitCode = 0;
    if (!isElevated()) {
      utilityExitCode = requireElevation(mode.c_str());
    } else if (mode == L"firewall-add" && appPath.empty()) {
      print(L"ERROR: --firewall-add requires --app PATH.\n");
      utilityExitCode = 2;
    } else {
      HRESULT hr = S_OK;
      if (mode == L"firewall-add") {
        hr = addFirewallRule(appPath);
      } else if (mode == L"firewall-remove") {
        hr = removeFirewallRule();
      } else {
        hr = uninstallPayloadPreflight();
      }
      if (FAILED(hr)) {
        print(L"%s failed: %s\n",
              mode == L"uninstall-preflight" ? L"Uninstall preflight" : L"Firewall operation",
              rcwin::hrMessage(hr).c_str());
        utilityExitCode = 1;
      } else {
        if (mode == L"firewall-add") {
          print(L"  firewall      : %s\n",
                hr == S_FALSE
                    ? L"private TCP 7890 rule stored and verified; no private profile is active"
                    : L"private TCP 7890 rule installed, effective, and verified");
        } else if (mode == L"firewall-remove") {
          print(L"  firewall      : %s\n", hr == S_FALSE ? L"already absent" : L"removed");
        } else {
          print(L"  payload       : critical files are available for removal\n");
        }
      }
    }
    ::CoUninitialize();
    return utilityExitCode;
  }

  const HRESULT mfHr = ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
  if (FAILED(mfHr)) {
    print(L"MFStartup failed: %s\n", rcwin::hrMessage(mfHr).c_str());
    ::CoUninitialize();
    return 1;
  }

  int exitCode = 0;

  if (mode == L"status") {
    print(L"RemoteCam virtual camera status\n");
    print(L"  CLSID         : %s\n", rcwin::kSourceClsidString);
    print(L"  elevated      : %s\n", isElevated() ? L"yes" : L"no");

    std::wstring registered;
    if (SUCCEEDED(readRegisteredPath(registered))) {
      const bool exists = ::GetFileAttributesW(registered.c_str()) != INVALID_FILE_ATTRIBUTES;
      print(L"  InprocServer32: %s%s\n", registered.c_str(),
            exists ? L"" : L"   <-- FILE MISSING");
      if (!exists) exitCode = 1;
    } else {
      print(L"  InprocServer32: not registered\n");
      exitCode = 1;
    }
    if (reportEnumeration() != S_OK) exitCode = 1;

  } else if (mode == L"register") {
    if (!isElevated()) {
      exitCode = requireElevation(L"register");
    } else if (::GetFileAttributesW(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
      print(L"ERROR: %s does not exist. Build rc-vcam first, or pass --dll PATH.\n",
            dllPath.c_str());
      exitCode = 1;
    } else {
      print(L"Registering RemoteCam\n");
      print(L"  dll           : %s\n", dllPath.c_str());

      std::wstring previousPath;
      const HRESULT previousHr = readRegisteredPath(previousPath);
      if (FAILED(previousHr) &&
          previousHr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) &&
          previousHr != HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) {
        print(L"  registry      : could not read existing state: %s\n",
              rcwin::hrMessage(previousHr).c_str());
        exitCode = 1;
      }

      HRESULT hr = exitCode == 0 ? ensureLogDirectory() : E_FAIL;
      if (exitCode == 0 && FAILED(hr)) {
        print(L"  WARNING: log directory: %s\n", rcwin::hrMessage(hr).c_str());
      }

      hr = exitCode == 0 ? writeRegistry(dllPath) : E_FAIL;
      if (FAILED(hr)) {
        if (exitCode == 0) {
          print(L"  registry      : FAILED %s\n", rcwin::hrMessage(hr).c_str());
        }
        exitCode = 1;
      } else {
        print(L"  registry      : HKLM\\%s\n", rcwin::kInprocKeyPath);

        ComPtr<IMFVirtualCamera> camera;
        WCHAR* registeredSymbolicLink = nullptr;
        UINT32 registeredSymbolicLinkLength = 0;
        hr = createCamera(MFVirtualCameraLifetime_System, &camera);
        if (SUCCEEDED(hr)) hr = camera->Start(nullptr);
        if (SUCCEEDED(hr)) {
          hr = camera->GetAllocatedString(
              MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
              &registeredSymbolicLink, &registeredSymbolicLinkLength);
        }
        if (FAILED(hr)) {
          print(L"  camera        : FAILED %s\n", rcwin::hrMessage(hr).c_str());
          const HRESULT cleanupHr = camera.Get() != nullptr
                                        ? removeCameraAndConfirm(camera.Get())
                                        : E_UNEXPECTED;
          if (SUCCEEDED(cleanupHr)) {
            const HRESULT rollback = SUCCEEDED(previousHr)
                                         ? writeRegistry(previousPath)
                                         : removeRegistry();
            print(L"  registry      : %s after confirmed camera cleanup\n",
                  SUCCEEDED(rollback) ? L"restored previous state"
                                      : rcwin::hrMessage(rollback).c_str());
          } else {
            print(L"  camera cleanup: FAILED %s\n", rcwin::hrMessage(cleanupHr).c_str());
            print(L"  registry      : preserved so the camera retains an activatable source\n");
          }
          exitCode = 1;
        } else {
          print(L"  camera        : created (system lifetime, all users)\n");
          const HRESULT enumerationHr = reportEnumeration(registeredSymbolicLink);
          if (enumerationHr != S_OK) {
            print(L"  camera        : FAILED post-registration enumeration (%s)\n",
                  rcwin::hrMessage(enumerationHr).c_str());
            const HRESULT cleanupHr =
                removeCameraAndConfirm(camera.Get(), registeredSymbolicLink);
            if (SUCCEEDED(cleanupHr)) {
              const HRESULT rollback = SUCCEEDED(previousHr)
                                           ? writeRegistry(previousPath)
                                           : removeRegistry();
              print(L"  camera        : removed and exact symbolic link is absent\n");
              print(L"  registry      : %s after confirmed camera cleanup\n",
                    SUCCEEDED(rollback) ? L"restored previous state"
                                        : rcwin::hrMessage(rollback).c_str());
            } else {
              print(L"  camera cleanup: FAILED %s\n", rcwin::hrMessage(cleanupHr).c_str());
              print(L"  registry      : preserved so the camera retains an activatable source\n");
            }
            exitCode = 1;
          } else {
            print(L"\nDone. RemoteCam should now appear in every camera picker.\n");
          }
        }
        ::CoTaskMemFree(registeredSymbolicLink);
      }
    }

  } else if (mode == L"unregister") {
    if (!isElevated()) {
      exitCode = requireElevation(L"unregister");
    } else {
      print(L"Unregistering RemoteCam\n");
      ComPtr<IMFVirtualCamera> camera;
      HRESULT cameraHr = createCamera(MFVirtualCameraLifetime_System, &camera);
      if (SUCCEEDED(cameraHr)) {
        cameraHr = removeCameraAndConfirm(camera.Get());
        if (cameraHr == S_OK) {
          print(L"  camera        : removed and exact symbolic link is absent\n");
        } else if (FAILED(cameraHr)) {
          print(L"  camera        : FAILED removal/verification (%s)\n",
                rcwin::hrMessage(cameraHr).c_str());
        }
      } else {
        print(L"  camera        : FAILED to open for removal (%s)\n",
              rcwin::hrMessage(cameraHr).c_str());
      }

      if (SUCCEEDED(cameraHr)) {
        const HRESULT registryHr = removeRegistry();
        print(L"  registry      : %s\n",
              SUCCEEDED(registryHr) ? L"removed" : rcwin::hrMessage(registryHr).c_str());
        if (FAILED(registryHr)) exitCode = 1;
      } else {
        print(L"  registry      : preserved because camera absence was not confirmed\n");
        exitCode = 1;
      }
    }

  } else if (mode == L"session") {
    // Session lifetime leaves nothing behind: the camera exists only while this process
    // does. That makes it the right tool for iterating on the DLL, since a rebuild only
    // needs this process restarted rather than an elevated re-registration.
    if (!isElevated()) {
      exitCode = requireElevation(L"session");
    } else {
      ComPtr<IMFVirtualCamera> camera;
      HRESULT hr = createCamera(MFVirtualCameraLifetime_Session, &camera);
      if (SUCCEEDED(hr)) hr = camera->Start(nullptr);
      if (FAILED(hr)) {
        print(L"Failed to create session camera: %s\n", rcwin::hrMessage(hr).c_str());
        print(L"Is the CLSID registered? Run --status.\n");
        exitCode = 1;
      } else {
        print(L"Session camera running. Press Ctrl+C to remove it.\n");
        reportEnumeration();
        for (;;) ::Sleep(1000);
      }
    }

  } else {
    exitCode = usage();
  }

  ::MFShutdown();
  ::CoUninitialize();
  return exitCode;
}
