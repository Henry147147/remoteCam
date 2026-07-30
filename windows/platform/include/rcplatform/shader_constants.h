#ifndef RCPLATFORM_SHADER_CONSTANTS_H
#define RCPLATFORM_SHADER_CONSTANTS_H

#include "rc/transform.h"

namespace rcplatform {

// Four float4 rows: the first three are rc::destToSource verbatim, padded only at the
// end of each row for HLSL cbuffer alignment. The shader performs explicit dot
// products, avoiding the row-major/column-major transpose ambiguity of HLSL matrices.
struct alignas(16) TransformConstants {
  float row0[4]{};
  float row1[4]{};
  float row2[4]{};
  float inverseSourceSize[4]{};
};

static_assert(sizeof(TransformConstants) == 64);

TransformConstants packTransformConstants(const rc::TransformParams& params);

}  // namespace rcplatform

#endif  // RCPLATFORM_SHADER_CONSTANTS_H
