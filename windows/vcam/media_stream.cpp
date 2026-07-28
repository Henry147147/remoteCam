#include "media_stream.h"

#include <mferror.h>

#include <new>

#include "rcwin/hr.h"
#include "rcwin/nv12.h"
#include "media_source.h"

using Microsoft::WRL::ComPtr;

namespace rcvcam {
namespace {

// Frame duration in 100 ns units, the unit Media Foundation uses everywhere.
constexpr LONGLONG kFrameDuration = 10000000LL * kFpsDenominator / kFpsNumerator;

}  // namespace

HRESULT MediaStream::Create(IMFMediaSource* source, IMFStreamDescriptor* descriptor,
                            IMFAttributes* attributes, MediaStream** out) {
  RC_RETURN_IF_NULL(out);
  *out = nullptr;

  MediaStream* stream = new (std::nothrow) MediaStream();
  RC_RETURN_HR_IF(stream == nullptr, E_OUTOFMEMORY);

  const HRESULT hr = stream->Initialize(source, descriptor, attributes);
  if (FAILED(hr)) {
    stream->Release();
    return hr;
  }
  *out = stream;
  return S_OK;
}

MediaStream::~MediaStream() {
  if (stopEvent_) ::CloseHandle(stopEvent_);
}

HRESULT MediaStream::Initialize(IMFMediaSource* source, IMFStreamDescriptor* descriptor,
                                IMFAttributes* attributes) {
  RC_RETURN_IF_NULL(source);
  RC_RETURN_IF_NULL(descriptor);

  source_ = source;
  descriptor_ = descriptor;
  attributes_ = attributes;

  RC_RETURN_IF_FAILED(::MFCreateEventQueue(&eventQueue_));

  stopEvent_ = ::CreateEventW(nullptr, TRUE /* manual reset */, FALSE, nullptr);
  RC_RETURN_HR_IF(stopEvent_ == nullptr, RC_HR_FROM_LAST_ERROR());
  return S_OK;
}

// --- IUnknown ---------------------------------------------------------------

IFACEMETHODIMP MediaStream::QueryInterface(REFIID riid, void** ppv) {
  RC_RETURN_IF_NULL(ppv);
  *ppv = nullptr;

  if (riid == IID_IUnknown || riid == IID_IMFMediaEventGenerator ||
      riid == IID_IMFMediaStream || riid == IID_IMFMediaStream2) {
    *ppv = static_cast<IMFMediaStream2*>(this);
  } else {
    return E_NOINTERFACE;
  }
  AddRef();
  return S_OK;
}

IFACEMETHODIMP_(ULONG) MediaStream::AddRef() {
  return static_cast<ULONG>(::InterlockedIncrement(&refCount_));
}

IFACEMETHODIMP_(ULONG) MediaStream::Release() {
  const long count = ::InterlockedDecrement(&refCount_);
  if (count == 0) delete this;
  return static_cast<ULONG>(count);
}

// --- IMFMediaEventGenerator -------------------------------------------------
//
// Every method here delegates to the event queue, which is Media Foundation's own
// thread-safe implementation. Rolling our own would be a pure liability.

IFACEMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
  ComPtr<IMFMediaEventQueue> queue;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    RC_RETURN_HR_IF(shutdown_, MF_E_SHUTDOWN);
    queue = eventQueue_;
  }
  return queue->BeginGetEvent(callback, state);
}

IFACEMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
  ComPtr<IMFMediaEventQueue> queue;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    RC_RETURN_HR_IF(shutdown_, MF_E_SHUTDOWN);
    queue = eventQueue_;
  }
  return queue->EndGetEvent(result, event);
}

IFACEMETHODIMP MediaStream::GetEvent(DWORD flags, IMFMediaEvent** event) {
  // The queue is copied out under the lock and the (potentially blocking) call is made
  // without it. GetEvent with MF_EVENT_FLAG_NO_WAIT clear blocks until an event
  // arrives, and holding the lock across that would deadlock against the timer thread
  // trying to queue the very sample being waited for.
  ComPtr<IMFMediaEventQueue> queue;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    RC_RETURN_HR_IF(shutdown_, MF_E_SHUTDOWN);
    queue = eventQueue_;
  }
  return queue->GetEvent(flags, event);
}

IFACEMETHODIMP MediaStream::QueueEvent(MediaEventType met, REFGUID extendedType,
                                       HRESULT status, const PROPVARIANT* value) {
  ComPtr<IMFMediaEventQueue> queue;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    RC_RETURN_HR_IF(shutdown_, MF_E_SHUTDOWN);
    queue = eventQueue_;
  }
  return queue->QueueEventParamVar(met, extendedType, status, value);
}

// --- IMFMediaStream ---------------------------------------------------------

IFACEMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** source) {
  RC_RETURN_IF_NULL(source);
  std::lock_guard<std::mutex> lock(mutex_);
  RC_RETURN_HR_IF(shutdown_, MF_E_SHUTDOWN);
  RC_RETURN_HR_IF(!source_, MF_E_SHUTDOWN);
  return source_.CopyTo(source);
}

IFACEMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor) {
  RC_RETURN_IF_NULL(descriptor);
  std::lock_guard<std::mutex> lock(mutex_);
  RC_RETURN_HR_IF(shutdown_, MF_E_SHUTDOWN);
  return descriptor_.CopyTo(descriptor);
}

IFACEMETHODIMP MediaStream::RequestSample(IUnknown* token) {
  std::lock_guard<std::mutex> lock(mutex_);
  RC_RETURN_HR_IF(shutdown_, MF_E_SHUTDOWN);
  RC_RETURN_HR_IF(!running_, MF_E_INVALIDREQUEST);

  // A null token is legal and common; it still has to occupy a slot so that requests
  // and delivered samples stay one-to-one.
  tokens_.emplace_back(token);
  return S_OK;
}

// --- IMFMediaStream2 --------------------------------------------------------

IFACEMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE state) {
  switch (state) {
    case MF_STREAM_STATE_RUNNING:
      return Start();
    case MF_STREAM_STATE_PAUSED:
    case MF_STREAM_STATE_STOPPED:
      return Stop();
    default:
      return MF_E_INVALID_STATE_TRANSITION;
  }
}

IFACEMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* state) {
  RC_RETURN_IF_NULL(state);
  std::lock_guard<std::mutex> lock(mutex_);
  RC_RETURN_HR_IF(shutdown_, MF_E_SHUTDOWN);
  *state = running_ ? MF_STREAM_STATE_RUNNING : MF_STREAM_STATE_STOPPED;
  return S_OK;
}

// --- lifecycle --------------------------------------------------------------

HRESULT MediaStream::Start() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    RC_RETURN_HR_IF(shutdown_, MF_E_SHUTDOWN);
    if (running_) return S_OK;
    running_ = true;
    tokens_.clear();
  }

  ::ResetEvent(stopEvent_);
  frames_.start();
  thread_ = std::thread(&MediaStream::ThreadMain, this);
  RC_LOG(L"stream started (%ux%u @ %u/%u)", kWidth, kHeight, kFpsNumerator, kFpsDenominator);
  return S_OK;
}

HRESULT MediaStream::Stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return S_OK;
    running_ = false;
  }

  ::SetEvent(stopEvent_);
  if (thread_.joinable()) thread_.join();
  frames_.stop();

  {
    std::lock_guard<std::mutex> lock(mutex_);
    tokens_.clear();
  }
  RC_LOG(L"stream stopped");
  return S_OK;
}

HRESULT MediaStream::Shutdown() {
  Stop();

  ComPtr<IMFMediaEventQueue> queue;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return S_OK;
    shutdown_ = true;
    queue = eventQueue_;
    eventQueue_.Reset();
    descriptor_.Reset();
    attributes_.Reset();
    // Releasing the source here is what breaks the deliberate reference cycle noted in
    // the header. Without it neither object is ever destroyed.
    source_.Reset();
  }
  if (queue) queue->Shutdown();
  return S_OK;
}

// --- sample production ------------------------------------------------------

HRESULT MediaStream::ProduceSample(uint64_t frameIndex, LONGLONG timestamp,
                                   IMFSample** out) {
  RC_RETURN_IF_NULL(out);
  *out = nullptr;

  ComPtr<IMFMediaBuffer> buffer;
  RC_RETURN_IF_FAILED(::MFCreate2DMediaBuffer(kWidth, kHeight, MFVideoFormat_NV12.Data1,
                                              FALSE, &buffer));

  ComPtr<IMF2DBuffer2> buffer2d;
  RC_RETURN_IF_FAILED(buffer.As(&buffer2d));

  BYTE* scanline0 = nullptr;
  LONG pitch = 0;
  BYTE* bufferStart = nullptr;
  DWORD bufferLength = 0;
  RC_RETURN_IF_FAILED(buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline0, &pitch,
                                           &bufferStart, &bufferLength));

  // A negative pitch means the buffer is bottom-up. NV12 from MFCreate2DMediaBuffer is
  // always top-down, so this would signal a Media Foundation change rather than a bug
  // here -- but writing through it as if it were positive would corrupt memory outside
  // the buffer, so refuse instead of guessing.
  if (pitch <= 0) {
    buffer2d->Unlock2D();
    RC_ERR(L"unexpected non-positive pitch %ld from MFCreate2DMediaBuffer", pitch);
    return E_UNEXPECTED;
  }

  const rcwin::Nv12Layout layout =
      rcwin::nv12Layout(static_cast<int>(kWidth), static_cast<int>(kHeight),
                        static_cast<int>(pitch));
  frames_.fill(scanline0, layout, frameIndex);

  RC_RETURN_IF_FAILED(buffer2d->Unlock2D());

  DWORD contiguous = 0;
  RC_RETURN_IF_FAILED(buffer2d->GetContiguousLength(&contiguous));
  RC_RETURN_IF_FAILED(buffer->SetCurrentLength(contiguous));

  ComPtr<IMFSample> sample;
  RC_RETURN_IF_FAILED(::MFCreateSample(&sample));
  RC_RETURN_IF_FAILED(sample->AddBuffer(buffer.Get()));
  RC_RETURN_IF_FAILED(sample->SetSampleTime(timestamp));
  RC_RETURN_IF_FAILED(sample->SetSampleDuration(kFrameDuration));

  return sample.CopyTo(out);
}

void MediaStream::ThreadMain() {
  ::SetThreadDescription(::GetCurrentThread(), L"rc-vcam frames");

  // High-resolution timers are the difference between honest 30 fps and the ~15 ms
  // granularity of the default timer, which would make every frame interval a coin
  // flip between 33 and 47 ms. The flag needs Windows 10 1803+; the fallback keeps
  // older builds working badly rather than not at all.
  HANDLE timer = ::CreateWaitableTimerExW(nullptr, nullptr,
                                          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
  if (!timer) timer = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  if (!timer) {
    RC_ERR(L"CreateWaitableTimerEx failed: %s",
           rcwin::hrMessage(rcwin::hrFromLastError()).c_str());
    return;
  }

  const LONGLONG startTime = ::MFGetSystemTime();
  uint64_t frameIndex = 0;
  HANDLE waits[2] = {stopEvent_, timer};

  for (;;) {
    // Scheduled against an absolute origin rather than by sleeping a fixed interval,
    // so a late wake-up does not push every subsequent frame later. Over an hour a
    // relative sleep would drift by minutes.
    const LONGLONG target = startTime + static_cast<LONGLONG>(frameIndex + 1) * kFrameDuration;
    LONGLONG delta = target - ::MFGetSystemTime();
    if (delta < 0) delta = 0;

    LARGE_INTEGER due;
    due.QuadPart = -delta;  // negative == relative
    if (!::SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) break;

    const DWORD wait = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (wait == WAIT_OBJECT_0) break;  // stopEvent_
    if (wait != WAIT_OBJECT_0 + 1) break;

    ++frameIndex;

    ComPtr<IUnknown> token;
    bool haveRequest = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!running_ || shutdown_) break;
      if (!tokens_.empty()) {
        token = tokens_.front();
        tokens_.pop_front();
        haveRequest = true;
      }
    }
    // No outstanding request means the consumer has not asked for a frame yet. Ticking
    // on regardless is what keeps the frame index -- and therefore the moving region of
    // the test pattern -- advancing in real time rather than in consumer time.
    if (!haveRequest) continue;

    ComPtr<IMFSample> sample;
    if (FAILED(ProduceSample(frameIndex, ::MFGetSystemTime(), &sample))) continue;

    if (token) sample->SetUnknown(MFSampleExtension_Token, token.Get());

    ComPtr<IMFMediaEventQueue> queue;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (shutdown_) break;
      queue = eventQueue_;
    }
    if (queue) {
      queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
    }
  }

  ::CloseHandle(timer);
}

}  // namespace rcvcam
