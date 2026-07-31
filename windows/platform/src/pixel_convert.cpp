#include "rcplatform/pixel_convert.h"

#include <algorithm>
#include <limits>

namespace rcplatform {
namespace {

uint8_t clampByte(int value) {
  return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

uint8_t luma(uint8_t blue, uint8_t green, uint8_t red) {
  // Fixed-point studio-range Rec.709. White maps to 235 and black to 16.
  return clampByte(16 + ((16 * blue + 157 * green + 47 * red + 128) >> 8));
}

int divideRounded256(int value) {
  return value >= 0 ? (value + 128) / 256 : -((-value + 128) / 256);
}

uint8_t chromaU(int blue, int green, int red) {
  return clampByte(128 + divideRounded256(113 * blue - 87 * green - 26 * red));
}

uint8_t chromaV(int blue, int green, int red) {
  return clampByte(128 + divideRounded256(-10 * blue - 102 * green + 112 * red));
}

bool requiredBytes(uint32_t stride, uint32_t rows, size_t& out) {
  if (rows != 0 && stride > std::numeric_limits<size_t>::max() / rows) return false;
  out = static_cast<size_t>(stride) * rows;
  return true;
}

}  // namespace

HRESULT bgraToNv12(const uint8_t* bgra, size_t bgraBytes, uint32_t bgraStride,
                   uint32_t width, uint32_t height, uint8_t* nv12,
                   size_t nv12Bytes, uint32_t nv12Stride) {
  if (bgra == nullptr || nv12 == nullptr) return E_POINTER;
  if (width == 0 || height == 0 || (width & 1u) != 0 || (height & 1u) != 0) {
    return E_INVALIDARG;
  }
  if (width > std::numeric_limits<uint32_t>::max() / 4u ||
      bgraStride < width * 4u || nv12Stride < width) {
    return E_INVALIDARG;
  }

  size_t requiredBgra = 0;
  size_t yBytes = 0;
  size_t uvBytes = 0;
  if (!requiredBytes(bgraStride, height, requiredBgra) ||
      !requiredBytes(nv12Stride, height, yBytes) ||
      !requiredBytes(nv12Stride, height / 2u, uvBytes) ||
      yBytes > std::numeric_limits<size_t>::max() - uvBytes) {
    return E_INVALIDARG;
  }
  if (bgraBytes < requiredBgra || nv12Bytes < yBytes + uvBytes) {
    return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
  }

  for (uint32_t row = 0; row < height; ++row) {
    const uint8_t* source = bgra + static_cast<size_t>(row) * bgraStride;
    uint8_t* destination = nv12 + static_cast<size_t>(row) * nv12Stride;
    for (uint32_t column = 0; column < width; ++column) {
      const uint8_t* pixel = source + static_cast<size_t>(column) * 4u;
      destination[column] = luma(pixel[0], pixel[1], pixel[2]);
    }
  }

  uint8_t* uvPlane = nv12 + yBytes;
  for (uint32_t row = 0; row < height; row += 2u) {
    const uint8_t* first = bgra + static_cast<size_t>(row) * bgraStride;
    const uint8_t* second = first + bgraStride;
    uint8_t* destination = uvPlane + static_cast<size_t>(row / 2u) * nv12Stride;
    for (uint32_t column = 0; column < width; column += 2u) {
      const uint8_t* p0 = first + static_cast<size_t>(column) * 4u;
      const uint8_t* p1 = p0 + 4u;
      const uint8_t* p2 = second + static_cast<size_t>(column) * 4u;
      const uint8_t* p3 = p2 + 4u;
      const int blue = (p0[0] + p1[0] + p2[0] + p3[0] + 2) / 4;
      const int green = (p0[1] + p1[1] + p2[1] + p3[1] + 2) / 4;
      const int red = (p0[2] + p1[2] + p2[2] + p3[2] + 2) / 4;
      destination[column] = chromaU(blue, green, red);
      destination[column + 1u] = chromaV(blue, green, red);
    }
  }
  return S_OK;
}

}  // namespace rcplatform
