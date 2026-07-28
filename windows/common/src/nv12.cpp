#include "rcwin/nv12.h"

#include <algorithm>
#include <cstring>

namespace rcwin {

Nv12Layout nv12Layout(int width, int height, int stride) {
  Nv12Layout out;
  if (width <= 0 || height <= 0) return out;

  // Round down to even. Rounding up would write past the caller's allocation.
  out.width = width & ~1;
  out.height = height & ~1;
  out.stride = stride > 0 ? stride : out.width;
  out.uvOffset = static_cast<size_t>(out.stride) * static_cast<size_t>(out.height);
  out.totalSize = out.uvOffset + out.uvOffset / 2;
  return out;
}

void nv12Fill(uint8_t* dst, const Nv12Layout& layout, Yuv colour) {
  if (!dst || layout.totalSize == 0) return;

  for (int row = 0; row < layout.height; ++row) {
    std::memset(dst + static_cast<size_t>(row) * layout.stride, colour.y,
                static_cast<size_t>(layout.width));
  }

  uint8_t* uv = dst + layout.uvOffset;
  for (int row = 0; row < layout.height / 2; ++row) {
    uint8_t* line = uv + static_cast<size_t>(row) * layout.stride;
    for (int col = 0; col < layout.width / 2; ++col) {
      line[col * 2 + 0] = colour.u;
      line[col * 2 + 1] = colour.v;
    }
  }
}

void nv12FillRect(uint8_t* dst, const Nv12Layout& layout, int x, int y, int w, int h,
                  Yuv colour) {
  if (!dst || layout.totalSize == 0 || w <= 0 || h <= 0) return;

  const int x0 = std::max(0, x);
  const int y0 = std::max(0, y);
  const int x1 = std::min(layout.width, x + w);
  const int y1 = std::min(layout.height, y + h);
  if (x0 >= x1 || y0 >= y1) return;

  for (int row = y0; row < y1; ++row) {
    std::memset(dst + static_cast<size_t>(row) * layout.stride + x0, colour.y,
                static_cast<size_t>(x1 - x0));
  }

  // Snap outward to whole 2x2 chroma blocks. Snapping inward would leave a one-pixel
  // fringe of the previous colour's chroma along odd edges, which reads as a coloured
  // halo and looks like a conversion bug rather than a rounding choice.
  const int cx0 = x0 / 2;
  const int cy0 = y0 / 2;
  const int cx1 = std::min(layout.width / 2, (x1 + 1) / 2);
  const int cy1 = std::min(layout.height / 2, (y1 + 1) / 2);

  uint8_t* uv = dst + layout.uvOffset;
  for (int row = cy0; row < cy1; ++row) {
    uint8_t* line = uv + static_cast<size_t>(row) * layout.stride;
    for (int col = cx0; col < cx1; ++col) {
      line[col * 2 + 0] = colour.u;
      line[col * 2 + 1] = colour.v;
    }
  }
}

}  // namespace rcwin
