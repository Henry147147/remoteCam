// rc-vcam-register.exe -- install-time registration for the virtual camera.
//
// Two separate things have to happen before Windows will load rc-vcam.dll:
//
//   1. The CLSID must resolve to the DLL, under HKLM\SOFTWARE\Classes\CLSID\{...}.
//   2. MFCreateVirtualCamera must be told that CLSID represents a camera.
//
// Both need administrator rights, once, at install time -- never at app launch. The
// running application must never require elevation.

#include <windows.h>

#include <aclapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfvirtualcamera.h>
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
HRESULT reportEnumeration() {
  ComPtr<IMFAttributes> attributes;
  RC_RETURN_IF_FAILED(::MFCreateAttributes(&attributes, 1));
  RC_RETURN_IF_FAILED(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                          MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));

  IMFActivate** devices = nullptr;
  UINT32 count = 0;
  RC_RETURN_IF_FAILED(::MFEnumDeviceSources(attributes.Get(), &devices, &count));

  bool found = false;
  print(L"  cameras (MF): %u\n", count);
  for (UINT32 i = 0; i < count; ++i) {
    WCHAR* name = nullptr;
    UINT32 length = 0;
    if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name,
                                                 &length))) {
      const bool ours = wcscmp(name, rcwin::kFriendlyName) == 0;
      found = found || ours;
      print(L"    %s%s\n", name, ours ? L"   <-- RemoteCam" : L"");
      ::CoTaskMemFree(name);
    }
    devices[i]->Release();
  }
  ::CoTaskMemFree(devices);

  print(L"  RemoteCam enumerates: %s\n", found ? L"YES" : L"NO");
  return found ? S_OK : S_FALSE;
}

int usage() {
  print(
      L"rc-vcam-register -- RemoteCam virtual camera registration\n"
      L"\n"
      L"  --status                 report registration state (no elevation needed)\n"
      L"  --register [--dll PATH]  register the CLSID and create the camera  [admin]\n"
      L"  --unregister             remove the camera and the CLSID           [admin]\n"
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
  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    if (arg == L"--dll" && i + 1 < argc) {
      dllPath = argv[++i];
    } else if (arg.rfind(L"--", 0) == 0) {
      mode = arg.substr(2);
    }
  }
  if (mode.empty()) return usage();

  const HRESULT comHr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(comHr)) {
    print(L"CoInitializeEx failed: %s\n", rcwin::hrMessage(comHr).c_str());
    return 1;
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
        hr = createCamera(MFVirtualCameraLifetime_System, &camera);
        if (SUCCEEDED(hr)) hr = camera->Start(nullptr);
        if (FAILED(hr)) {
          print(L"  camera        : FAILED %s\n", rcwin::hrMessage(hr).c_str());
          const HRESULT rollback = SUCCEEDED(previousHr)
                                       ? writeRegistry(previousPath)
                                       : removeRegistry();
          print(L"  registry      : %s after camera failure\n",
                SUCCEEDED(rollback) ? L"restored previous state"
                                    : rcwin::hrMessage(rollback).c_str());
          exitCode = 1;
        } else {
          print(L"  camera        : created (system lifetime, all users)\n");
          reportEnumeration();
          print(L"\nDone. RemoteCam should now appear in every camera picker.\n");
        }
      }
    }

  } else if (mode == L"unregister") {
    if (!isElevated()) {
      exitCode = requireElevation(L"unregister");
    } else {
      print(L"Unregistering RemoteCam\n");
      ComPtr<IMFVirtualCamera> camera;
      HRESULT hr = createCamera(MFVirtualCameraLifetime_System, &camera);
      if (SUCCEEDED(hr)) {
        hr = camera->Remove();
        print(L"  camera        : %s\n", SUCCEEDED(hr) ? L"removed"
                                                       : rcwin::hrMessage(hr).c_str());
      } else {
        print(L"  camera        : not present (%s)\n", rcwin::hrMessage(hr).c_str());
      }

      hr = removeRegistry();
      print(L"  registry      : %s\n",
            SUCCEEDED(hr) ? L"removed" : rcwin::hrMessage(hr).c_str());
      if (FAILED(hr)) exitCode = 1;
      reportEnumeration();
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
