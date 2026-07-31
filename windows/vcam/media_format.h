#ifndef RC_VCAM_MEDIA_FORMAT_H
#define RC_VCAM_MEDIA_FORMAT_H

#include <windows.h>

#include <mfidl.h>

#include <array>

namespace rcvcam {

struct VideoFormat {
  UINT32 width = 0;
  UINT32 height = 0;
  UINT32 fpsNumerator = 0;
  UINT32 fpsDenominator = 0;

  friend constexpr bool operator==(const VideoFormat&, const VideoFormat&) = default;
};

// The fixed public contract from PLAN.md: six NV12 canvas sizes, each at 30 and
// 60 fps. The order is the source's preference order. The default remains 1080p30 so
// consumers that accept the current media type do not unexpectedly request 4K60.
inline constexpr std::array<VideoFormat, 12> kVideoFormats = {{
    {1920, 1080, 30, 1},
    {1920, 1080, 60, 1},
    {3840, 2160, 30, 1},
    {3840, 2160, 60, 1},
    {2560, 1440, 30, 1},
    {2560, 1440, 60, 1},
    {1280, 720, 30, 1},
    {1280, 720, 60, 1},
    {960, 540, 30, 1},
    {960, 540, 60, 1},
    {640, 480, 30, 1},
    {640, 480, 60, 1},
}};

inline constexpr VideoFormat kDefaultVideoFormat = kVideoFormats[0];

constexpr bool isSupportedVideoFormat(const VideoFormat& format) {
  for (const VideoFormat& supported : kVideoFormats) {
    if (format == supported) return true;
  }
  return false;
}

constexpr LONGLONG frameDuration100ns(const VideoFormat& format) {
  return format.fpsNumerator == 0
             ? 0
             : 10000000LL * format.fpsDenominator / format.fpsNumerator;
}

HRESULT createVideoMediaType(const VideoFormat& format, IMFMediaType** out);
HRESULT videoFormatFromMediaType(IMFMediaType* type, VideoFormat& out);

}  // namespace rcvcam

#endif  // RC_VCAM_MEDIA_FORMAT_H
