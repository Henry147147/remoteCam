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
          std::memcpy(dst + static_cast<size_t>(row) * layout.stride,
                      scratch.data() + static_cast<size_t>(row) * src.stride,
                      static_cast<size_t>(copyBytes));
        }
        for (int row = 0; row < layout.height / 2; ++row) {
          std::memcpy(dst + layout.uvOffset + static_cast<size_t>(row) * layout.stride,
                      scratch.data() + src.uvOffset + static_cast<size_t>(row) * src.stride,
                      static_cast<size_t>(copyBytes));
        }
        live = true;
      } else if (!mismatchLogged_) {
        // Geometry negotiation is an M2 concern; until the pipeline can rescale, a
        // mismatched producer is a configuration error worth saying out loud rather
        // than silently showing a scrambled image. Once, though -- the producer will
        // keep publishing the wrong size thirty times a second until someone fixes it.
        mismatchLogged_ = true;
        RC_WARN(L"ring frame is %ux%u fourcc 0x%08X, expected %dx%d NV12 -- ignoring "
                L"(further mismatches will not be logged)",
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
