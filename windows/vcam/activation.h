#ifndef RC_VCAM_ACTIVATION_H
#define RC_VCAM_ACTIVATION_H

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <mutex>

#include "module_lock.h"

namespace rcvcam {

class Activation final : public IMFActivate {
 public:
  static HRESULT CreateInstance(REFIID riid, void** ppv);

  IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
  IFACEMETHODIMP_(ULONG) AddRef() override;
  IFACEMETHODIMP_(ULONG) Release() override;

  IFACEMETHODIMP GetItem(REFGUID key, PROPVARIANT* value) override;
  IFACEMETHODIMP GetItemType(REFGUID key, MF_ATTRIBUTE_TYPE* type) override;
  IFACEMETHODIMP CompareItem(REFGUID key, REFPROPVARIANT value, BOOL* result) override;
  IFACEMETHODIMP Compare(IMFAttributes* theirs, MF_ATTRIBUTES_MATCH_TYPE matchType,
                        BOOL* result) override;
  IFACEMETHODIMP GetUINT32(REFGUID key, UINT32* value) override;
  IFACEMETHODIMP GetUINT64(REFGUID key, UINT64* value) override;
  IFACEMETHODIMP GetDouble(REFGUID key, double* value) override;
  IFACEMETHODIMP GetGUID(REFGUID key, GUID* value) override;
  IFACEMETHODIMP GetStringLength(REFGUID key, UINT32* length) override;
  IFACEMETHODIMP GetString(REFGUID key, LPWSTR value, UINT32 size, UINT32* length) override;
  IFACEMETHODIMP GetAllocatedString(REFGUID key, LPWSTR* value, UINT32* length) override;
  IFACEMETHODIMP GetBlobSize(REFGUID key, UINT32* size) override;
  IFACEMETHODIMP GetBlob(REFGUID key, UINT8* value, UINT32 size, UINT32* blobSize) override;
  IFACEMETHODIMP GetAllocatedBlob(REFGUID key, UINT8** value, UINT32* size) override;
  IFACEMETHODIMP GetUnknown(REFGUID key, REFIID riid, LPVOID* ppv) override;
  IFACEMETHODIMP SetItem(REFGUID key, REFPROPVARIANT value) override;
  IFACEMETHODIMP DeleteItem(REFGUID key) override;
  IFACEMETHODIMP DeleteAllItems() override;
  IFACEMETHODIMP SetUINT32(REFGUID key, UINT32 value) override;
  IFACEMETHODIMP SetUINT64(REFGUID key, UINT64 value) override;
  IFACEMETHODIMP SetDouble(REFGUID key, double value) override;
  IFACEMETHODIMP SetGUID(REFGUID key, REFGUID value) override;
  IFACEMETHODIMP SetString(REFGUID key, LPCWSTR value) override;
  IFACEMETHODIMP SetBlob(REFGUID key, const UINT8* value, UINT32 size) override;
  IFACEMETHODIMP SetUnknown(REFGUID key, IUnknown* value) override;
  IFACEMETHODIMP LockStore() override;
  IFACEMETHODIMP UnlockStore() override;
  IFACEMETHODIMP GetCount(UINT32* count) override;
  IFACEMETHODIMP GetItemByIndex(UINT32 index, GUID* key, PROPVARIANT* value) override;
  IFACEMETHODIMP CopyAllItems(IMFAttributes* destination) override;

  IFACEMETHODIMP ActivateObject(REFIID riid, void** ppv) override;
  IFACEMETHODIMP ShutdownObject() override;
  IFACEMETHODIMP DetachObject() override;

 private:
  Activation() = default;
  ~Activation() = default;
  HRESULT Initialize();

  ModuleLock moduleLock_;
  long refCount_ = 1;
  std::mutex mutex_;
  Microsoft::WRL::ComPtr<IMFAttributes> attributes_;
  Microsoft::WRL::ComPtr<IUnknown> source_;
};

}  // namespace rcvcam

#endif
