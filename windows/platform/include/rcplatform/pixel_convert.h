#ifndef RCPLATFORM_PIXEL_CONVERT_H
#define RCPLATFORM_PIXEL_CONVERT_H

#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace rcplatform {

// Converts one top-down BGRA8 frame to studio-range Rec.709 NV12. Both dimensions
// must be even. The explicit sizes and strides keep mapped D3D11 row padding from
// becoming an out-of-bounds read or a diagonally sheared camera frame.
HRESULT bgraToNv12(const uint8_t* bgra, size_t bgraBytes, uint32_t bgraStride,
                   uint32_t width, uint32_t height, uint8_t* nv12,
                   size_t nv12Bytes, uint32_t nv12Stride);

}  // namespace rcplatform

#endif  // RCPLATFORM_PIXEL_CONVERT_H
