#include "frame_source.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "rcwin/hr.h"
#include "rcwin/test_pattern.h"

namespace rcvcam {

void FrameSource::start() {
  const HRESULT hr = ring_.create();
  ringReady_ = SUCCEEDED(hr);
  if (!ringReady_) {
    RC_WARN(L"frame ring unavailable (%s); serving placeholder frames only",
            rcwin::hrMessage(hr).c_str());
  }
}

void FrameSource::stop() {
  ring_.close();
  ringReady_ = false;
  lastWasLive_ = false;
  everLogged_ = false;
  mismatchLogged_ = false;  // warn again next session, not once per process lifetime
}

bool FrameSource::fill(uint8_t* dst, const rcwin::Nv12Layout& layout, uint64_t frameIndex) {
  bool live = false;

  // Tell the producer what this consumer actually picked, so a mismatch is something it
  // can correct rather than something we silently drop frames over.
  if (ringReady_ && (layout.width != requestedWidth_ || layout.height != requestedHeight_)) {
    const HRESULT hr = ring_.requestGeometry(static_cast<uint32_t>(layout.width),
                                             static_cast<uint32_t>(layout.height),
                                             rcwin::kFourccNv12);
    if (SUCCEEDED(hr)) {
      requestedWidth_ = layout.width;
      requestedHeight_ = layout.height;
    } else {
      RC_WARN(L"could not publish requested geometry: %s", rcwin::hrMessage(hr).c_str());
    }
  }

  if (ringReady_ && ring_.millisSinceLastWrite() <= kStaleAfterMillis) {
    // Staged through a scratch buffer rather than read straight into `dst`, because the
    // producer's stride is its own business and need not match the stride Media
    // Foundation handed us. Copying row by row below is what keeps a padded MF buffer
    // from shearing the image diagonally.
    static thread_local std::vector<uint8_t> scratch;
    if (scratch.size() < rcwin::kRingSlotBytes) scratch.resize(rcwin::kRingSlotBytes);

    rcwin::FrameInfo info;
    if (ring_.readLatest(scratch.data(), static_cast<uint32_t>(scratch.size()), info) == S_OK) {
      if (info.width == static_cast<uint32_t>(layout.width) &&
          info.height == static_cast<uint32_t>(layout.height) &&
          info.format == rcwin::kFourccNv12) {
        const rcwin::Nv12Layout src =
            rcwin::nv12Layout(static_cast<int>(info.width), static_cast<int>(info.height),
                              static_cast<int>(info.stride));
        const int copyBytes = std::min(src.stride, layout.stride);

        for (int row = 0; row < layout.height; ++row) {
          uint8_t* destination = dst + static_cast<size_t>(row) * layout.stride;
          std::memcpy(destination,
                      scratch.data() + static_cast<size_t>(row) * src.stride,
                      static_cast<size_t>(copyBytes));
          if (copyBytes < layout.stride) {
            std::memset(destination + copyBytes, 16,
                        static_cast<size_t>(layout.stride - copyBytes));
          }
        }
        for (int row = 0; row < layout.height / 2; ++row) {
          uint8_t* destination =
              dst + layout.uvOffset + static_cast<size_t>(row) * layout.stride;
          std::memcpy(destination,
                      scratch.data() + src.uvOffset + static_cast<size_t>(row) * src.stride,
                      static_cast<size_t>(copyBytes));
          if (copyBytes < layout.stride) {
            std::memset(destination + copyBytes, 128,
                        static_cast<size_t>(layout.stride - copyBytes));
          }
        }
        if (mismatchLogged_) {
          mismatchLogged_ = false;
          RC_LOG(L"ring frame format matches the virtual camera again");
        }
        live = true;
      } else if (!mismatchLogged_) {
        // The requested geometry is published above, so a producer that keeps sending
        // the wrong size is ignoring it rather than guessing wrongly. Still only warn
        // once -- it would otherwise arrive thirty times a second.
        mismatchLogged_ = true;
        RC_WARN(L"ring frame is %ux%u fourcc 0x%08X, but %dx%d NV12 was requested -- "
                L"ignoring (further mismatches will not be logged)",
                info.width, info.height, info.format, layout.width, layout.height);
      }
    }
  }

  if (!live) {
    rcwin::renderPattern(dst, layout, frameIndex, rcwin::PatternStyle::Placeholder);
  }

  if (!everLogged_ || live != lastWasLive_) {
    RC_LOG(L"frame source -> %s", live ? L"ring (live)" : L"placeholder");
    lastWasLive_ = live;
    everLogged_ = true;
  }
  return live;
}

}  // namespace rcvcam
