#ifndef RCPLATFORM_FRAME_RING_SINK_H
#define RCPLATFORM_FRAME_RING_SINK_H

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "rcplatform/video_pipeline.h"

namespace rcplatform {

enum class FrameRingSinkState {
  Stopped,
  WaitingForConsumer,
  AdaptingGeometry,
  Publishing,
  ProducerConflict,
  Failed,
};

struct FrameRingSinkSnapshot {
  FrameRingSinkState state = FrameRingSinkState::Stopped;
  uint32_t width = 1920;
  uint32_t height = 1080;
  uint64_t publishedFrames = 0;
  uint64_t droppedFrames = 0;
  HRESULT error = S_OK;
};

struct BgraPreviewFrame {
  std::vector<uint8_t> pixels;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
  uint64_t ptsMicros = 0;
};

// Copies transformed BGRA textures into a three-slot staging queue. GPU completion,
// CPU colour conversion and the cross-session ring write happen on a separate worker,
// so a camera consumer can never stall the decoder/transform thread.
class FrameRingSink final : public IFrameSink {
 public:
  using Observer = std::function<void(const FrameRingSinkSnapshot&)>;
  using PreviewObserver = std::function<void(std::shared_ptr<const BgraPreviewFrame>)>;

  FrameRingSink();
  ~FrameRingSink() override;

  FrameRingSink(const FrameRingSink&) = delete;
  FrameRingSink& operator=(const FrameRingSink&) = delete;

  void start();
  void stop();
  void setObserver(Observer observer);
  void setPreviewObserver(PreviewObserver observer);
  FrameRingSinkSnapshot snapshot() const;

  // Geometry most recently requested by the active virtual-camera consumer. Before a
  // consumer opens, this is the 1080p default.
  std::pair<uint32_t, uint32_t> desiredGeometry() const;

  HRESULT publish(const TextureFrame& frame) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rcplatform

#endif  // RCPLATFORM_FRAME_RING_SINK_H
