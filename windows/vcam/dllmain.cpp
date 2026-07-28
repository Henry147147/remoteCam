// COM entry points for rc-vcam.dll.
//
// This DLL is never loaded by RemoteCam's own application. The Media Foundation Frame
// Server -- svchost.exe -k Camera, LOCAL SERVICE, Session 0 -- CoCreateInstances our
// CLSID when some consumer opens the camera. Everything here therefore runs as a
// service, with no console, no window station, and no user to show an error to. That
// is why the very first thing DllMain does is name the log.

#include <windows.h>

#include <new>

#include "rcwin/guids.h"
#include "rcwin/hr.h"
#include "media_source.h"

namespace {

long g_objectCount = 0;

class ClassFactory final : public IClassFactory {
 public:
  IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IClassFactory) {
      *ppv = static_cast<IClassFactory*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  IFACEMETHODIMP_(ULONG) AddRef() override {
    return static_cast<ULONG>(::InterlockedIncrement(&refCount_));
  }

  IFACEMETHODIMP_(ULONG) Release() override {
    const long count = ::InterlockedDecrement(&refCount_);
    if (count == 0) delete this;
    return static_cast<ULONG>(count);
  }

  IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    // Aggregation is not supported and never will be: a media source has too much
    // internal threading for an outer object to safely control its lifetime.
    if (outer) return CLASS_E_NOAGGREGATION;
    return rcvcam::MediaSource::CreateInstance(riid, ppv);
  }

  IFACEMETHODIMP LockServer(BOOL lock) override {
    if (lock) {
      ::InterlockedIncrement(&g_objectCount);
    } else {
      ::InterlockedDecrement(&g_objectCount);
    }
    return S_OK;
  }

 private:
  long refCount_ = 1;
};

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
  UNREFERENCED_PARAMETER(reserved);
  switch (reason) {
    case DLL_PROCESS_ATTACH:
      // No thread notifications: we spawn our own threads and have no per-thread state,
      // so the callbacks would be pure overhead on a process as thread-heavy as svchost.
      ::DisableThreadLibraryCalls(module);
      rcwin::logInit(L"rc-vcam");
      RC_LOG(L"rc-vcam.dll loaded into pid %u (%s)", ::GetCurrentProcessId(),
             rcwin::modulePath().c_str());
      break;
    case DLL_PROCESS_DETACH:
      break;
    default:
      break;
  }
  return TRUE;
}

_Check_return_ STDAPI DllGetClassObject(_In_ REFCLSID rclsid, _In_ REFIID riid,
                                        _Outptr_ LPVOID* ppv) {
  if (!ppv) return E_POINTER;
  *ppv = nullptr;

  if (rclsid != rcwin::kSourceClsid) {
    RC_WARN(L"DllGetClassObject for an unknown CLSID");
    return CLASS_E_CLASSNOTAVAILABLE;
  }

  ClassFactory* factory = new (std::nothrow) ClassFactory();
  if (!factory) return E_OUTOFMEMORY;

  const HRESULT hr = factory->QueryInterface(riid, ppv);
  factory->Release();
  return hr;
}

__control_entrypoint(DllExport) STDAPI DllCanUnloadNow() {
  return ::InterlockedCompareExchange(&g_objectCount, 0, 0) == 0 ? S_OK : S_FALSE;
}
