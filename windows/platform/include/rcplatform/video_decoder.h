#ifndef RCPLATFORM_VIDEO_DECODER_H
#define RCPLATFORM_VIDEO_DECODER_H

#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <memory>

#include "rc/annexb.h"

namespace rcplatform {

struct TextureFrame {
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
  uint32_t arraySlice = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t ptsMicros = 0;
};

class IVideoDecoder {
 public:
  virtual ~IVideoDecoder() = default;
  // S_OK produces a frame, S_FALSE means the decoder buffered the access unit and
  // needs more input, failure means reset/request-keyframe.
  virtual HRESULT decode(const uint8_t* annexB, size_t size, uint64_t ptsMicros,
                         TextureFrame& out) = 0;
  virtual void flush() = 0;
};

// Available only when RC_WITH_FFMPEG was enabled at configure time. The factory stays
// linkable without FFmpeg and returns E_NOTIMPL, which lets the rest of the app report
// a precise unavailable reason instead of using preprocessor conditionals everywhere.
HRESULT createFfmpegD3D11Decoder(rc::annexb::Codec codec,
                                 std::unique_ptr<IVideoDecoder>& out);

}  // namespace rcplatform

#endif  // RCPLATFORM_VIDEO_DECODER_H
