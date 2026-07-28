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

namespace rcvcam {

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

  HRESULT Start();
  HRESULT Stop();
  HRESULT Shutdown();

 private:
  MediaStream() = default;
  ~MediaStream();

  HRESULT Initialize(IMFMediaSource* source, IMFStreamDescriptor* descriptor,
                     IMFAttributes* attributes);
  void ThreadMain();
  HRESULT ProduceSample(uint64_t frameIndex, LONGLONG timestamp, IMFSample** out);

  mutable std::mutex mutex_;
  long refCount_ = 1;
  bool shutdown_ = false;
  bool running_ = false;

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
