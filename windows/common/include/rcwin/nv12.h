// NV12 plane geometry.
//
// NV12 is the format the whole Windows side speaks: it is what the DXVA decoder
// produces, what MF virtual cameras are expected to advertise, and what every consumer
// in the compatibility matrix accepts without a converter in the path.
//
// Layout: a full-resolution Y plane, then an interleaved UV plane at half resolution
// in both axes. Both planes share the same stride. Chroma is 4:2:0, so one UV pair
// covers a 2x2 block of luma -- which is why every dimension here must be even.

#ifndef RCWIN_NV12_H
#define RCWIN_NV12_H

#include <cstddef>
#include <cstdint>

namespace rcwin {

struct Nv12Layout {
  int width = 0;
  int height = 0;
  int stride = 0;        // bytes per row, identical for both planes
  size_t uvOffset = 0;   // byte offset of the UV plane from the start of the buffer
  size_t totalSize = 0;  // stride * height * 3 / 2
};

// `stride` of 0 means packed (stride == width). Media Foundation hands us a negative
// or padded stride often enough that it is worth carrying explicitly rather than
// assuming: a wrong stride produces a skewed, diagonally-sheared image, which is the
// single most recognisable bring-up bug in this format.
Nv12Layout nv12Layout(int width, int height, int stride = 0);

// A colour in BT.601 studio-range YUV, which is what NV12 carries. Stored as YUV
// rather than RGB so the pattern generator never converts at runtime -- conversion is
// exactly where an off-by-one in range handling hides.
struct Yuv {
  uint8_t y = 16;
  uint8_t u = 128;
  uint8_t v = 128;
};

// Fills the whole buffer with one colour.
void nv12Fill(uint8_t* dst, const Nv12Layout& layout, Yuv colour);

// Fills an axis-aligned rectangle, clipped to the frame. Coordinates are in luma
// pixels; the chroma write is derived, so odd coordinates snap outward to the
// enclosing 2x2 block rather than tearing a half-populated chroma sample.
void nv12FillRect(uint8_t* dst, const Nv12Layout& layout, int x, int y, int w, int h,
                  Yuv colour);

}  // namespace rcwin

#endif  // RCWIN_NV12_H
