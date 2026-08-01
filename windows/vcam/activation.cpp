#include "activation.h"

#include "media_source.h"
#include "rcwin/hr.h"

namespace rcvcam {

HRESULT Activation::CreateInstance(REFIID riid, void** ppv) {
  RC_RETURN_IF_NULL(ppv);
  *ppv = nullptr;
  auto* activation = new (std::nothrow) Activation();
  if (!activation) return E_OUTOFMEMORY;
  const HRESULT init = activation->Initialize();
  const HRESULT result = SUCCEEDED(init) ? activation->QueryInterface(riid, ppv) : init;
  activation->Release();
  return result;
}

HRESULT Activation::Initialize() { return ::MFCreateAttributes(&attributes_, 8); }

IFACEMETHODIMP Activation::QueryInterface(REFIID riid, void** ppv) {
  RC_RETURN_IF_NULL(ppv);
  *ppv = nullptr;
  if (riid == IID_IUnknown || riid == IID_IMFAttributes || riid == IID_IMFActivate) {
    *ppv = static_cast<IMFActivate*>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

IFACEMETHODIMP_(ULONG) Activation::AddRef() {
  return static_cast<ULONG>(::InterlockedIncrement(&refCount_));
}
IFACEMETHODIMP_(ULONG) Activation::Release() {
  const long count = ::InterlockedDecrement(&refCount_);
  if (count == 0) delete this;
  return static_cast<ULONG>(count);
}

#define RC_ATTR(method, signature, arguments) \
  IFACEMETHODIMP Activation::method signature { return attributes_->method arguments; }
RC_ATTR(GetItem, (REFGUID k, PROPVARIANT* v), (k, v))
RC_ATTR(GetItemType, (REFGUID k, MF_ATTRIBUTE_TYPE* t), (k, t))
RC_ATTR(CompareItem, (REFGUID k, REFPROPVARIANT v, BOOL* r), (k, v, r))
RC_ATTR(Compare, (IMFAttributes* a, MF_ATTRIBUTES_MATCH_TYPE m, BOOL* r), (a, m, r))
RC_ATTR(GetUINT32, (REFGUID k, UINT32* v), (k, v))
RC_ATTR(GetUINT64, (REFGUID k, UINT64* v), (k, v))
RC_ATTR(GetDouble, (REFGUID k, double* v), (k, v))
RC_ATTR(GetGUID, (REFGUID k, GUID* v), (k, v))
RC_ATTR(GetStringLength, (REFGUID k, UINT32* n), (k, n))
RC_ATTR(GetString, (REFGUID k, LPWSTR v, UINT32 s, UINT32* n), (k, v, s, n))
RC_ATTR(GetAllocatedString, (REFGUID k, LPWSTR* v, UINT32* n), (k, v, n))
RC_ATTR(GetBlobSize, (REFGUID k, UINT32* s), (k, s))
RC_ATTR(GetBlob, (REFGUID k, UINT8* v, UINT32 s, UINT32* n), (k, v, s, n))
RC_ATTR(GetAllocatedBlob, (REFGUID k, UINT8** v, UINT32* s), (k, v, s))
RC_ATTR(GetUnknown, (REFGUID k, REFIID i, LPVOID* v), (k, i, v))
RC_ATTR(SetItem, (REFGUID k, REFPROPVARIANT v), (k, v))
RC_ATTR(DeleteItem, (REFGUID k), (k))
RC_ATTR(DeleteAllItems, (), ())
RC_ATTR(SetUINT32, (REFGUID k, UINT32 v), (k, v))
RC_ATTR(SetUINT64, (REFGUID k, UINT64 v), (k, v))
RC_ATTR(SetDouble, (REFGUID k, double v), (k, v))
RC_ATTR(SetGUID, (REFGUID k, REFGUID v), (k, v))
RC_ATTR(SetString, (REFGUID k, LPCWSTR v), (k, v))
RC_ATTR(SetBlob, (REFGUID k, const UINT8* v, UINT32 s), (k, v, s))
RC_ATTR(SetUnknown, (REFGUID k, IUnknown* v), (k, v))
RC_ATTR(LockStore, (), ())
RC_ATTR(UnlockStore, (), ())
RC_ATTR(GetCount, (UINT32* c), (c))
RC_ATTR(GetItemByIndex, (UINT32 i, GUID* k, PROPVARIANT* v), (i, k, v))
RC_ATTR(CopyAllItems, (IMFAttributes* d), (d))
#undef RC_ATTR

IFACEMETHODIMP Activation::ActivateObject(REFIID riid, void** ppv) {
  RC_RETURN_IF_NULL(ppv);
  *ppv = nullptr;
  std::lock_guard<std::mutex> lock(mutex_);
  if (!source_) {
    RC_RETURN_IF_FAILED(MediaSource::CreateInstance(IID_IUnknown, &source_));
  }
  return source_->QueryInterface(riid, ppv);
}

IFACEMETHODIMP Activation::ShutdownObject() { return E_NOTIMPL; }
IFACEMETHODIMP Activation::DetachObject() { return E_NOTIMPL; }

}  // namespace rcvcam
