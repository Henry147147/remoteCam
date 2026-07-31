#include "rcplatform/shader_constants.h"

namespace rcplatform {

TransformConstants packTransformConstants(const rc::TransformParams& params) {
  const rc::Mat3 matrix = rc::destToSource(params);
  TransformConstants constants;
  constants.row0[0] = matrix.m[0];
  constants.row0[1] = matrix.m[1];
  constants.row0[2] = matrix.m[2];
  constants.row1[0] = matrix.m[3];
  constants.row1[1] = matrix.m[4];
  constants.row1[2] = matrix.m[5];
  constants.row2[0] = matrix.m[6];
  constants.row2[1] = matrix.m[7];
  constants.row2[2] = matrix.m[8];
  if (params.srcWidth > 0 && params.srcHeight > 0) {
    constants.inverseSourceSize[0] = 1.0f / static_cast<float>(params.srcWidth);
    constants.inverseSourceSize[1] = 1.0f / static_cast<float>(params.srcHeight);
    constants.inverseSourceSize[2] = static_cast<float>(params.srcWidth);
    constants.inverseSourceSize[3] = static_cast<float>(params.srcHeight);
  }
  return constants;
}

}  // namespace rcplatform
