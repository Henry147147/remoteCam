#include "rcplatform/video_pipeline.h"

namespace rcplatform {

VideoPipeline::VideoPipeline(IVideoDecoder& decoder, IFrameTransform& transform,
                             IEffectChain* effects, IFrameSink& sink)
    : decoder_(decoder), transform_(transform), effects_(effects), sink_(sink) {}

PipelineOutcome VideoPipeline::push(const EncodedAccessUnit& unit,
                                    const rc::TransformParams& transformParams) {
  const rc::annexb::AccessUnitReport report =
      rc::annexb::inspect(unit.bytes, unit.size, unit.codec);
  if (!report.isAnnexB) return {PipelineResult::InvalidAnnexB, E_INVALIDARG};
  if (unit.keyframe && (!report.hasKeyframeSlice || !report.hasParameterSets)) {
    return {PipelineResult::KeyframeMissingParameterSets, E_INVALIDARG};
  }

  TextureFrame decoded;
  const HRESULT decodeHr = decoder_.decode(unit.bytes, unit.size, unit.ptsMicros, decoded);
  if (decodeHr == S_FALSE) return {PipelineResult::DecoderNeedsMoreInput, S_FALSE};
  if (FAILED(decodeHr)) return {PipelineResult::DecodeFailed, decodeHr};
  // The wire header is authoritative. A decoder is free to reorder internally, but it
  // must not replace the timestamp with a codec time base or drop it at the API seam.
  decoded.ptsMicros = unit.ptsMicros;

  TextureFrame transformed;
  const HRESULT transformHr = transform_.apply(decoded, transformParams, transformed);
  if (FAILED(transformHr)) return {PipelineResult::TransformFailed, transformHr};
  transformed.ptsMicros = decoded.ptsMicros;

  if (effects_ != nullptr) {
    const HRESULT effectHr = effects_->apply(transformed);
    if (FAILED(effectHr)) return {PipelineResult::EffectFailed, effectHr};
  }

  const HRESULT sinkHr = sink_.publish(transformed);
  if (FAILED(sinkHr)) return {PipelineResult::SinkFailed, sinkHr};
  return {PipelineResult::Published, S_OK};
}

}  // namespace rcplatform
