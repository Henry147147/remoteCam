#ifndef RCPLATFORM_D3D11_TRANSFORM_H
#define RCPLATFORM_D3D11_TRANSFORM_H

#include <memory>

#include "rcplatform/video_pipeline.h"

namespace rcplatform {

// Converts an NV12 decoder texture to an upright/cropped BGRA render target in one
// pixel-shader pass. The exact rc::destToSource matrix reaches the shader through
// packTransformConstants; no geometry is re-derived in HLSL.
class D3D11TransformPass final : public IFrameTransform {
 public:
  D3D11TransformPass();
  ~D3D11TransformPass() override;

  D3D11TransformPass(const D3D11TransformPass&) = delete;
  D3D11TransformPass& operator=(const D3D11TransformPass&) = delete;

  HRESULT apply(const TextureFrame& input, const rc::TransformParams& params,
                TextureFrame& out) override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rcplatform

#endif  // RCPLATFORM_D3D11_TRANSFORM_H
