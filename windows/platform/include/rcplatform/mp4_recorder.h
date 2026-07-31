#ifndef RCPLATFORM_MP4_RECORDER_H
#define RCPLATFORM_MP4_RECORDER_H

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "rcplatform/frame_ring_sink.h"

namespace rcplatform {

struct Mp4RecorderConfig {
  std::wstring outputPath;
  uint32_t width = 1920;
  uint32_t height = 1080;
  uint32_t fpsNumerator = 30;
  uint32_t fpsDenominator = 1;
  uint32_t bitrateBitsPerSecond = 12'000'000;

  // BGRA frames are immutable shared objects, but they are still full-resolution.
  // Keeping this deliberately small bounds both latency and memory when an encoder
  // falls behind. On overflow the oldest queued frame is dropped and the newest is
  // retained, so recording stays close to live time.
  size_t queueCapacity = 4;
};

enum class Mp4RecorderState {
  Idle,
  Starting,
  Recording,
  Finalizing,
  Completed,
  Failed,
};

enum class Mp4EncoderPath {
  Unknown,
  HardwareTransformsEnabled,
  SoftwareOnly,
};

struct Mp4RecorderSnapshot {
  Mp4RecorderState state = Mp4RecorderState::Idle;
  Mp4EncoderPath encoderPath = Mp4EncoderPath::Unknown;
  uint64_t submittedFrames = 0;
  uint64_t writtenFrames = 0;
  uint64_t droppedFrames = 0;
  uint64_t duration100ns = 0;
  size_t queuedFrames = 0;
  HRESULT error = S_OK;
  std::wstring outputPath;

  // Non-empty only while recording or when an already-finalized file could not be
  // atomically renamed. A rename failure preserves that playable file for recovery;
  // encoder and muxer failures remove their incomplete partial file.
  std::wstring partialPath;
};

class IMp4Recorder {
 public:
  // State callbacks run on the thread that caused the transition (including the
  // recorder worker). They must return promptly and marshal UI work to its owner
  // thread; `snapshot()` is safe from a callback, while `start()`/`stop()` are not
  // re-entrant operations.
  using Observer = std::function<void(const Mp4RecorderSnapshot&)>;

  virtual ~IMp4Recorder() = default;

  // Starts a worker and returns once the request is accepted. Media Foundation setup
  // happens on that worker; observe Starting -> Recording or Failed for its result.
  virtual HRESULT start(const Mp4RecorderConfig& config) = 0;

  // Enqueues one canonical BGRA preview frame without waiting for conversion or the
  // encoder. S_FALSE means an older queued frame was dropped to accept this one.
  virtual HRESULT enqueue(std::shared_ptr<const BgraPreviewFrame> frame) = 0;

  // Drains the bounded queue, finalizes the MP4, and atomically replaces outputPath.
  // This is synchronous so a successful return guarantees the final path is usable.
  virtual HRESULT stop() = 0;

  virtual Mp4RecorderSnapshot snapshot() const = 0;
  virtual void setObserver(Observer observer) = 0;
};

std::unique_ptr<IMp4Recorder> createMp4Recorder();

}  // namespace rcplatform

#endif  // RCPLATFORM_MP4_RECORDER_H
