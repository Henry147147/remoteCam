// The single video stream exposed by the media source.
//
// Media Foundation media sources are pull-driven: the Frame Server calls RequestSample
// and we answer with an MEMediaSample event. That is deliberately decoupled from
// pacing here -- a dedicated timer thread ticks at the advertised frame rate and only
// emits when a request is outstanding, so the effective rate is
// min(consumer request rate, advertised rate) and a slow consumer can never make us
// buffer frames it has not asked for.

#ifndef RC_VCAM_MEDIA_STREAM_H
#define RC_VCAM_MEDIA_STREAM_H

#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <deque>
#include <mutex>
#include <thread>

#include "frame_source.h"
#include "media_format.h"
#include "module_lock.h"

namespace rcvcam {

// Ceiling on outstanding RequestSample calls. Eight is a quarter-second of buffering at
// the advertised rate -- deep enough for a consumer that primes a pipeline with several
// requests up front, shallow enough that a runaway one cannot grow the Frame Server's
// working set.
inline constexpr size_t kMaxPendingRequests = 8;

class MediaStream final : public IMFMediaStream2 {
 public:
  static HRESULT Create(IMFMediaSource* source, IMFStreamDescriptor* descriptor,
                        IMFAttributes* attributes, MediaStream** out);

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

  // IMFMediaStream
  IFACEMETHODIMP GetMediaSource(IMFMediaSource** source) override;
  IFACEMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** descriptor) override;
  IFACEMETHODIMP RequestSample(IUnknown* token) override;

  // IMFMediaStream2
  IFACEMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
  IFACEMETHODIMP GetStreamState(MF_STREAM_STATE* state) override;

  // Applies the media type selected on the presentation descriptor passed to
  // IMFMediaSource::Start. Media Foundation requires clients to change it only while
  // stopped; changing an active stream is refused rather than racing buffer geometry.
  HRESULT Configure(const VideoFormat& format, IMFMediaType* mediaType);
  HRESULT Start();
  HRESULT Stop();
  HRESULT Shutdown();

 private:
  MediaStream() = default;
  ~MediaStream();

  HRESULT Initialize(IMFMediaSource* source, IMFStreamDescriptor* descriptor,
                     IMFAttributes* attributes);
  void ThreadMain();
  HRESULT ProduceSample(const VideoFormat& format, uint64_t frameIndex,
                        LONGLONG timestamp, IMFSample** out);

  // A consumer may hold the stream after releasing the source, so the stream keeps the
  // module alive in its own right rather than relying on the source to do it. Declared
  // first so it outlives the thread below.
  ModuleLock moduleLock_;

  mutable std::mutex mutex_;
  // Start/Stop perform work outside mutex_ (thread join and ring teardown). Serialise
  // those transitions separately so concurrent Media Foundation lifecycle calls
  // cannot both join the same std::thread or start after a completed Stop.
  std::mutex lifecycleMutex_;
  long refCount_ = 1;
  bool shutdown_ = false;
  bool running_ = false;
  bool requestOverflowLogged_ = false;
  bool sampleFailureLogged_ = false;
  VideoFormat format_ = kDefaultVideoFormat;

  Microsoft::WRL::ComPtr<IMFMediaEventQueue> eventQueue_;
  Microsoft::WRL::ComPtr<IMFStreamDescriptor> descriptor_;
  Microsoft::WRL::ComPtr<IMFAttributes> attributes_;

  // Strong reference back to the owning source, as Media Foundation requires
  // GetMediaSource to keep working for as long as a consumer holds the stream. It is
  // a reference cycle by construction; MediaSource::Shutdown breaks it by calling
  // Shutdown() here, which is guaranteed because the Frame Server always shuts a
  // source down before releasing it.
  Microsoft::WRL::ComPtr<IMFMediaSource> source_;

  std::deque<Microsoft::WRL::ComPtr<IUnknown>> tokens_;
  std::thread thread_;
  HANDLE stopEvent_ = nullptr;

  FrameSource frames_;
};

}  // namespace rcvcam

#endif  // RC_VCAM_MEDIA_STREAM_H
