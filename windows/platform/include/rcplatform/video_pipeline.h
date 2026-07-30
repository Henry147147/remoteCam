#ifndef RCPLATFORM_VIDEO_PIPELINE_H
#define RCPLATFORM_VIDEO_PIPELINE_H

#include <windows.h>

#include <cstddef>
#include <cstdint>

#include "rc/annexb.h"
#include "rc/transform.h"
#include "rcplatform/video_decoder.h"

namespace rcplatform {

struct EncodedAccessUnit {
  const uint8_t* bytes = nullptr;
  size_t size = 0;
  rc::annexb::Codec codec = rc::annexb::Codec::H264;
  bool keyframe = false;
  uint64_t ptsMicros = 0;
};

class IFrameTransform {
 public:
  virtual ~IFrameTransform() = default;
  virtual HRESULT apply(const TextureFrame& input, const rc::TransformParams& params,
                        TextureFrame& out) = 0;
};

class IEffectChain {
 public:
  virtual ~IEffectChain() = default;
  virtual HRESULT apply(TextureFrame& frame) = 0;
};

class IFrameSink {
 public:
  virtual ~IFrameSink() = default;
  virtual HRESULT publish(const TextureFrame& frame) = 0;
};

enum class PipelineResult {
  Published,
  InvalidAnnexB,
  KeyframeMissingParameterSets,
  DecoderNeedsMoreInput,
  DecodeFailed,
  TransformFailed,
  EffectFailed,
  SinkFailed,
};

struct PipelineOutcome {
  PipelineResult result = PipelineResult::Published;
  HRESULT detail = S_OK;
};

// Synchronous by design: the decoder's D3D11 array slice remains reserved only for
// this call. Order is decoder -> geometric transform -> effects -> sink; applying AI
// effects before rotation feeds them a sideways person and materially degrades them.
class VideoPipeline {
 public:
  VideoPipeline(IVideoDecoder& decoder, IFrameTransform& transform, IEffectChain* effects,
                IFrameSink& sink);

  PipelineOutcome push(const EncodedAccessUnit& unit,
                       const rc::TransformParams& transformParams);

 private:
  IVideoDecoder& decoder_;
  IFrameTransform& transform_;
  IEffectChain* effects_ = nullptr;
  IFrameSink& sink_;
};

}  // namespace rcplatform

#endif  // RCPLATFORM_VIDEO_PIPELINE_H
