#include "media_source.h"

#include <mferror.h>

#include <array>
#include <new>

#include "media_format.h"
#include "rcwin/hr.h"

using Microsoft::WRL::ComPtr;

namespace rcvcam {
namespace {

HRESULT selectedVideoType(IMFPresentationDescriptor* presentation,
                          VideoFormat& format, IMFMediaType** out) {
  RC_RETURN_IF_NULL(presentation);
  RC_RETURN_IF_NULL(out);
  *out = nullptr;

  DWORD streamCount = 0;
  RC_RETURN_IF_FAILED(presentation->GetStreamDescriptorCount(&streamCount));
  RC_RETURN_HR_IF(streamCount != 1, MF_E_INVALIDSTREAMNUMBER);

  BOOL selected = FALSE;
  ComPtr<IMFStreamDescriptor> descriptor;
  RC_RETURN_IF_FAILED(
      presentation->GetStreamDescriptorByIndex(0, &selected, &descriptor));
  RC_RETURN_HR_IF(!selected, MF_E_MEDIA_SOURCE_NO_STREAMS_SELECTED);

  ComPtr<IMFMediaTypeHandler> handler;
  RC_RETURN_IF_FAILED(descriptor->GetMediaTypeHandler(&handler));
  ComPtr<IMFMediaType> type;
  RC_RETURN_IF_FAILED(handler->GetCurrentMediaType(&type));
  RC_RETURN_IF_FAILED(videoFormatFromMediaType(type.Get(), format));
  return type.CopyTo(out);
}

}  // namespace

HRESULT MediaSource::CreateInstance(REFIID riid, void** ppv) {
  RC_RETURN_IF_NULL(ppv);
  *ppv = nullptr;

  MediaSource* source = new (std::nothrow) MediaSource();
  RC_RETURN_HR_IF(source == nullptr, E_OUTOFMEMORY);

  HRESULT hr = source->Initialize();
  if (SUCCEEDED(hr)) hr = source->QueryInterface(riid, ppv);

  // Initialize may have got as far as building the stream, which takes a strong
  // reference back to this source -- the deliberate cycle documented in media_stream.h.
  // On a later failure, Release() alone would drop the count to one and leave both
  // objects alive for the lifetime of the Frame Server process. Shutdown breaks the
  // cycle, and is harmless if Initialize failed before the stream existed.
  if (FAILED(hr)) source->Shutdown();

  source->Release();
  return hr;
}

MediaSource::~MediaSource() = default;

HRESULT MediaSource::Initialize() {
  RC_RETURN_IF_FAILED(::MFCreateEventQueue(&eventQueue_));
  RC_RETURN_IF_FAILED(::MFCreateAttributes(&attributes_, 4));

  std::array<ComPtr<IMFMediaType>, kVideoFormats.size()> types;
  std::array<IMFMediaType*, kVideoFormats.size()> rawTypes{};
  IMFMediaType* defaultType = nullptr;
  for (size_t index = 0; index < kVideoFormats.size(); ++index) {
    RC_RETURN_IF_FAILED(createVideoMediaType(kVideoFormats[index], &types[index]));
    rawTypes[index] = types[index].Get();
    if (kVideoFormats[index] == kDefaultVideoFormat) defaultType = rawTypes[index];
  }
  RC_RETURN_HR_IF(defaultType == nullptr, E_UNEXPECTED);

  ComPtr<IMFStreamDescriptor> descriptor;
  RC_RETURN_IF_FAILED(::MFCreateStreamDescriptor(
      0, static_cast<DWORD>(rawTypes.size()), rawTypes.data(), &descriptor));

  ComPtr<IMFMediaTypeHandler> handler;
  RC_RETURN_IF_FAILED(descriptor->GetMediaTypeHandler(&handler));
  RC_RETURN_IF_FAILED(handler->SetCurrentMediaType(defaultType));

  // These four attributes are what make the Frame Server treat this object as a camera
  // rather than as an anonymous media source. A source missing them enumerates in the
  // device list and then fails to open, which reads as a permissions or registration
  // fault and sends the investigation somewhere else entirely.
  RC_RETURN_IF_FAILED(::MFCreateAttributes(&streamAttributes_, 8));
  RC_RETURN_IF_FAILED(streamAttributes_->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0));
  RC_RETURN_IF_FAILED(
      streamAttributes_->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, PINNAME_VIDEO_CAPTURE));
  RC_RETURN_IF_FAILED(streamAttributes_->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1));
  RC_RETURN_IF_FAILED(streamAttributes_->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES,
                                                   MFFrameSourceTypes_Color));

  // Mirrored onto the descriptor as well: different parts of the pipeline read the
  // category from different objects, and a mismatch between the two is worse than
  // either being absent.
  RC_RETURN_IF_FAILED(streamAttributes_->CopyAllItems(descriptor.Get()));

  MediaStream* stream = nullptr;
  RC_RETURN_IF_FAILED(MediaStream::Create(static_cast<IMFMediaSourceEx*>(this),
                                          descriptor.Get(), streamAttributes_.Get(),
                                          &stream));
  stream_.Attach(stream);

  IMFStreamDescriptor* descriptors[] = {descriptor.Get()};
  RC_RETURN_IF_FAILED(
      ::MFCreatePresentationDescriptor(ARRAYSIZE(descriptors), descriptors, &descriptor_));
  RC_RETURN_IF_FAILED(descriptor_->SelectStream(0));

  RC_LOG(L"media source initialised (%zu NV12 types; default %ux%u @ %u/%u)",
         kVideoFormats.size(), kDefaultVideoFormat.width, kDefaultVideoFormat.height,
         kDefaultVideoFormat.fpsNumerator, kDefaultVideoFormat.fpsDenominator);
  return S_OK;
}

HRESULT MediaSource::CheckShutdown() const {
  return shutdown_ ? MF_E_SHUTDOWN : S_OK;
}

// --- IUnknown ---------------------------------------------------------------

IFACEMETHODIMP MediaSource::QueryInterface(REFIID riid, void** ppv) {
  RC_RETURN_IF_NULL(ppv);
  *ppv = nullptr;

  if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator ||
      riid == IID_IMFMediaSource || riid == IID_IMFMediaSourceEx) {
    *ppv = static_cast<IMFMediaSourceEx*>(this);
  } else if (riid == IID_IMFGetService) {
    *ppv = static_cast<IMFGetService*>(this);
  } else if (riid == __uuidof(IKsControl)) {
    *ppv = static_cast<IKsControl*>(this);
  } else if (riid == IID_IMFSampleAllocatorControl) {
    *ppv = static_cast<IMFSampleAllocatorControl*>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

IFACEMETHODIMP_(ULONG) MediaSource::AddRef() {
  return static_cast<ULONG>(::InterlockedIncrement(&refCount_));
}

IFACEMETHODIMP_(ULONG) MediaSource::Release() {
  const long count = ::InterlockedDecrement(&refCount_);
  if (count == 0) delete this;
  return static_cast<ULONG>(count);
}

// --- IMFMediaEventGenerator -------------------------------------------------

IFACEMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
  ComPtr<IMFMediaEventQueue> queue;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    RC_RETURN_IF_FAILED(CheckShutdown());
    queue = eventQueue_;
  }
  return queue->BeginGetEvent(callback, state);
}

IFACEMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
  ComPtr<IMFMediaEventQueue> queue;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    RC_RETURN_IF_FAILED(CheckShutdown());
    queue = eventQueue_;
  }
  return queue->EndGetEvent(result, event);
}

IFACEMETHODIMP MediaSource::GetEvent(DWORD flags, IMFMediaEvent** event) {
  // Copied out under the lock, called without it -- a blocking GetEvent while holding
  // the source lock would deadlock against anything trying to queue an event.
  ComPtr<IMFMediaEventQueue> queue;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    RC_RETURN_IF_FAILED(CheckShutdown());
    queue = eventQueue_;
  }
  return queue->GetEvent(flags, event);
}

IFACEMETHODIMP MediaSource::QueueEvent(MediaEventType met, REFGUID extendedType,
                                       HRESULT status, const PROPVARIANT* value) {
  ComPtr<IMFMediaEventQueue> queue;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    RC_RETURN_IF_FAILED(CheckShutdown());
    queue = eventQueue_;
  }
  return queue->QueueEventParamVar(met, extendedType, status, value);
}

// --- IMFMediaSource ---------------------------------------------------------

IFACEMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** pd) {
  RC_RETURN_IF_NULL(pd);
  std::lock_guard<std::mutex> lock(mutex_);
  RC_RETURN_IF_FAILED(CheckShutdown());
  // A clone, not the original: consumers select and deselect streams on the descriptor
  // they are given, and handing out the source's own would let one consumer's choices
  // leak into another's.
  return descriptor_->Clone(pd);
}

IFACEMETHODIMP MediaSource::GetCharacteristics(DWORD* characteristics) {
  RC_RETURN_IF_NULL(characteristics);
  std::lock_guard<std::mutex> lock(mutex_);
  RC_RETURN_IF_FAILED(CheckShutdown());
  // Live, not seekable, no known duration -- the truthful description of a camera, and
  // what stops a consumer offering the user a scrub bar.
  *characteristics = MFMEDIASOURCE_IS_LIVE;
  return S_OK;
}

IFACEMETHODIMP MediaSource::Start(IMFPresentationDescriptor* pd, const GUID* timeFormat,
                                  const PROPVARIANT* startPosition) {
  RC_RETURN_IF_NULL(pd);
  RC_RETURN_HR_IF(timeFormat != nullptr && *timeFormat != GUID_NULL,
                  MF_E_UNSUPPORTED_TIME_FORMAT);

  VideoFormat selectedFormat;
  ComPtr<IMFMediaType> selectedType;
  RC_RETURN_IF_FAILED(selectedVideoType(pd, selectedFormat, &selectedType));

  ComPtr<IMFMediaEventQueue> queue;
  ComPtr<MediaStream> stream;
  bool wasStarted = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    RC_RETURN_IF_FAILED(CheckShutdown());
    queue = eventQueue_;
    stream = stream_;
    wasStarted = started_;
    started_ = true;
  }

  PROPVARIANT empty;
  ::PropVariantInit(&empty);
  const PROPVARIANT* position = startPosition ? startPosition : &empty;

  HRESULT hr = stream->Configure(selectedFormat, selectedType.Get());

  // Order is load-bearing. A consumer waits for MENewStream to learn the stream exists
  // and only then subscribes to it; queueing MESourceStarted first races it into
  // missing the stream entirely.
  if (SUCCEEDED(hr)) {
    hr = queue->QueueEventParamUnk(wasStarted ? MEUpdatedStream : MENewStream,
                                   GUID_NULL, S_OK,
                                   static_cast<IMFMediaStream2*>(stream.Get()));
  }
  if (SUCCEEDED(hr)) {
    hr = stream->QueueEvent(wasStarted ? MEStreamSeeked : MEStreamStarted, GUID_NULL, S_OK,
                            position);
  }
  if (SUCCEEDED(hr)) hr = stream->Start();
  if (SUCCEEDED(hr)) {
    hr = queue->QueueEventParamVar(wasStarted ? MESourceSeeked : MESourceStarted, GUID_NULL,
                                   S_OK, position);
  }

  ::PropVariantClear(&empty);
  if (FAILED(hr)) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!shutdown_) started_ = wasStarted;
    RC_ERR(L"Start failed: %s", rcwin::hrMessage(hr).c_str());
  }
  return hr;
}

IFACEMETHODIMP MediaSource::Stop() {
  ComPtr<IMFMediaEventQueue> queue;
  ComPtr<MediaStream> stream;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    RC_RETURN_IF_FAILED(CheckShutdown());
    queue = eventQueue_;
    stream = stream_;
  }

  if (stream) {
    stream->Stop();
    stream->QueueEvent(MEStreamStopped, GUID_NULL, S_OK, nullptr);
  }
  return queue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
}

IFACEMETHODIMP MediaSource::Pause() {
  ComPtr<IMFMediaEventQueue> queue;
  ComPtr<MediaStream> stream;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    RC_RETURN_IF_FAILED(CheckShutdown());
    queue = eventQueue_;
    stream = stream_;
  }

  if (stream) {
    stream->Stop();
    stream->QueueEvent(MEStreamPaused, GUID_NULL, S_OK, nullptr);
  }
  return queue->QueueEventParamVar(MESourcePaused, GUID_NULL, S_OK, nullptr);
}

IFACEMETHODIMP MediaSource::Shutdown() {
  ComPtr<IMFMediaEventQueue> queue;
  ComPtr<MediaStream> stream;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return MF_E_SHUTDOWN;
    shutdown_ = true;
    queue = eventQueue_;
    stream = stream_;
    eventQueue_.Reset();
    stream_.Reset();
    descriptor_.Reset();
    attributes_.Reset();
    streamAttributes_.Reset();
  }

  if (stream) stream->Shutdown();
  if (queue) queue->Shutdown();
  RC_LOG(L"media source shut down");
  return S_OK;
}

// --- IMFMediaSourceEx -------------------------------------------------------

IFACEMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** attributes) {
  RC_RETURN_IF_NULL(attributes);
  std::lock_guard<std::mutex> lock(mutex_);
  RC_RETURN_IF_FAILED(CheckShutdown());
  return attributes_.CopyTo(attributes);
}

IFACEMETHODIMP MediaSource::GetStreamAttributes(DWORD streamId, IMFAttributes** attributes) {
  RC_RETURN_IF_NULL(attributes);
  RC_RETURN_HR_IF(streamId != 0, MF_E_INVALIDSTREAMNUMBER);
  std::lock_guard<std::mutex> lock(mutex_);
  RC_RETURN_IF_FAILED(CheckShutdown());
  return streamAttributes_.CopyTo(attributes);
}

IFACEMETHODIMP MediaSource::SetD3DManager(IUnknown* manager) {
  // Frames are produced in system memory and staged through the shared-memory ring, so
  // there is no D3D device for the Frame Server to share with us. Accepting the call
  // and ignoring the manager is correct; failing it makes some consumers give up on
  // the device rather than fall back to system memory.
  UNREFERENCED_PARAMETER(manager);
  return S_OK;
}

// --- IMFGetService ----------------------------------------------------------

IFACEMETHODIMP MediaSource::GetService(REFGUID service, REFIID riid, LPVOID* ppv) {
  RC_RETURN_IF_NULL(ppv);
  *ppv = nullptr;
  if (service != GUID_NULL) return MF_E_UNSUPPORTED_SERVICE;
  return QueryInterface(riid, ppv);
}

// --- IKsControl -------------------------------------------------------------
//
// The Frame Server probes for camera control properties (exposure, white balance, and
// the extended-property set) through this interface. There is nothing to control yet;
// manual camera controls live on the phone and arrive in M4.
//
// ERROR_SET_NOT_FOUND is the specific refusal a KS client expects for "I do not
// implement that property set". Returning E_NOTIMPL or E_FAIL instead makes some
// consumers treat the device as broken and drop it, which presents as a camera that
// appears and then vanishes from the picker.

IFACEMETHODIMP MediaSource::KsProperty(PKSPROPERTY property, ULONG propertyLength, void* data,
                                       ULONG dataLength, ULONG* bytesReturned) {
  UNREFERENCED_PARAMETER(property);
  UNREFERENCED_PARAMETER(propertyLength);
  UNREFERENCED_PARAMETER(data);
  UNREFERENCED_PARAMETER(dataLength);
  if (bytesReturned) *bytesReturned = 0;
  return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

IFACEMETHODIMP MediaSource::KsMethod(PKSMETHOD method, ULONG methodLength, void* data,
                                     ULONG dataLength, ULONG* bytesReturned) {
  UNREFERENCED_PARAMETER(method);
  UNREFERENCED_PARAMETER(methodLength);
  UNREFERENCED_PARAMETER(data);
  UNREFERENCED_PARAMETER(dataLength);
  if (bytesReturned) *bytesReturned = 0;
  return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

IFACEMETHODIMP MediaSource::KsEvent(PKSEVENT event, ULONG eventLength, void* data,
                                    ULONG dataLength, ULONG* bytesReturned) {
  UNREFERENCED_PARAMETER(event);
  UNREFERENCED_PARAMETER(eventLength);
  UNREFERENCED_PARAMETER(data);
  UNREFERENCED_PARAMETER(dataLength);
  if (bytesReturned) *bytesReturned = 0;
  return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

// --- IMFSampleAllocatorControl ----------------------------------------------

IFACEMETHODIMP MediaSource::SetDefaultAllocator(DWORD outputStreamId, IUnknown* allocator) {
  UNREFERENCED_PARAMETER(outputStreamId);
  UNREFERENCED_PARAMETER(allocator);
  // Never called, because GetAllocatorUsage below declares that we allocate our own.
  return E_NOTIMPL;
}

IFACEMETHODIMP MediaSource::GetAllocatorUsage(DWORD outputStreamId, DWORD* inputStreamId,
                                              MFSampleAllocatorUsage* usage) {
  RC_RETURN_IF_NULL(usage);
  RC_RETURN_HR_IF(outputStreamId != 0, MF_E_INVALIDSTREAMNUMBER);
  if (inputStreamId) *inputStreamId = 0;
  // We allocate through MFCreate2DMediaBuffer. Declaring that here stops the Frame
  // Server handing us a D3D-backed allocator we would have to honour and cannot use
  // while frames arrive over shared memory as system memory.
  *usage = MFSampleAllocatorUsage_UsesCustomAllocator;
  return S_OK;
}

}  // namespace rcvcam
