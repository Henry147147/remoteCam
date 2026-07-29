// The COM media source the Frame Server instantiates.
//
// This object is created by CoCreateInstance from inside svchost.exe -k Camera, in
// Session 0, as LOCAL SERVICE -- not by RemoteCam's own application. Everything about
// its lifetime is therefore outside our control: it is created when some consumer
// opens the camera and torn down when the last one closes it, and there may be several
// consumers at once.

#ifndef RC_VCAM_MEDIA_SOURCE_H
#define RC_VCAM_MEDIA_SOURCE_H

#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>

// The KS headers must come AFTER everything that pulls in cguid.h (which mfidl.h does,
// via objbase.h). ks.h macro-defines GUID_NULL to __uuidof(struct GUID_NULL); if
// cguid.h is parsed afterwards, its own `extern const GUID GUID_NULL` declaration
// expands into that macro and fails to compile inside a Windows SDK header, which
// makes the error look like an SDK bug rather than an include-order one.
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>

#include <mutex>

#include "media_stream.h"
#include "module_lock.h"

namespace rcvcam {

// The one format advertised in M1. The full ladder from PLAN.md §1 lands once the
// Session 0 handoff is proven -- a single media type removes an entire class of
// negotiation failure from the first bring-up, and negotiation failures inside the
// Frame Server are the hardest kind to observe.
inline constexpr UINT32 kWidth = 1920;
inline constexpr UINT32 kHeight = 1080;
inline constexpr UINT32 kFpsNumerator = 30;
inline constexpr UINT32 kFpsDenominator = 1;

class MediaSource final : public IMFMediaSourceEx,
                          public IMFGetService,
                          public IKsControl,
                          public IMFSampleAllocatorControl {
 public:
  static HRESULT CreateInstance(REFIID riid, void** ppv);

  // IUnknown
  IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
  IFACEMETHODIMP_(ULONG) AddRef() override;
  IFACEMETHODIMP_(ULONG) Release() override;

  // IMFMediaEventGenerator
  IFACEMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
  IFACEMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
  IFACEMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
  IFACEMETHODIMP QueueEvent(MediaEventType met, REFGUID extendedType, HRESULT status,
                            const PROPVARIANT* value) override;

  // IMFMediaSource
  IFACEMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** pd) override;
  IFACEMETHODIMP GetCharacteristics(DWORD* characteristics) override;
  IFACEMETHODIMP Pause() override;
  IFACEMETHODIMP Shutdown() override;
  IFACEMETHODIMP Start(IMFPresentationDescriptor* pd, const GUID* timeFormat,
                       const PROPVARIANT* startPosition) override;
  IFACEMETHODIMP Stop() override;

  // IMFMediaSourceEx
  IFACEMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override;
  IFACEMETHODIMP GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) override;
  IFACEMETHODIMP SetD3DManager(IUnknown* manager) override;

  // IMFGetService
  IFACEMETHODIMP GetService(REFGUID service, REFIID riid, LPVOID* ppv) override;

  // IKsControl -- the Frame Server probes camera control properties through this.
  // Unimplemented properties must be refused politely; see the note in the .cpp.
  IFACEMETHODIMP KsProperty(PKSPROPERTY property, ULONG propertyLength, void* data,
                            ULONG dataLength, ULONG* bytesReturned) override;
  IFACEMETHODIMP KsMethod(PKSMETHOD method, ULONG methodLength, void* data, ULONG dataLength,
                          ULONG* bytesReturned) override;
  IFACEMETHODIMP KsEvent(PKSEVENT event, ULONG eventLength, void* data, ULONG dataLength,
                         ULONG* bytesReturned) override;

  // IMFSampleAllocatorControl
  IFACEMETHODIMP SetDefaultAllocator(DWORD outputStreamId, IUnknown* allocator) override;
  IFACEMETHODIMP GetAllocatorUsage(DWORD outputStreamId, DWORD* inputStreamId,
                                   MFSampleAllocatorUsage* usage) override;

 private:
  MediaSource() = default;
  ~MediaSource();

  HRESULT Initialize();
  HRESULT CheckShutdown() const;

  // Declared first so it is destroyed last: the module must outlive every other member
  // of this object, including the stream and its thread.
  ModuleLock moduleLock_;

  mutable std::mutex mutex_;
  long refCount_ = 1;
  bool shutdown_ = false;
  bool started_ = false;

  Microsoft::WRL::ComPtr<IMFMediaEventQueue> eventQueue_;
  Microsoft::WRL::ComPtr<IMFAttributes> attributes_;
  Microsoft::WRL::ComPtr<IMFAttributes> streamAttributes_;
  Microsoft::WRL::ComPtr<IMFPresentationDescriptor> descriptor_;
  Microsoft::WRL::ComPtr<MediaStream> stream_;
};

}  // namespace rcvcam

#endif  // RC_VCAM_MEDIA_SOURCE_H
