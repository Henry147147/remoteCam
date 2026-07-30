#include "rcplatform/abr_controller.h"

#include <algorithm>

namespace rcplatform {
namespace {

constexpr uint32_t kGrowthSamplesBeforeBackoff = 2;
constexpr uint32_t kStableSamplesBeforeRecovery = 10;  // five seconds at 2 Hz

uint64_t percent(uint64_t value, uint64_t numerator, uint64_t denominator) {
  // Bitrates are bounded to 100 Mbps by the control codec, so multiplication cannot
  // overflow. Keeping the arithmetic integer also makes target values deterministic.
  return value * numerator / denominator;
}

}  // namespace

AbrController::AbrController(uint64_t initialBitsPerSec, uint64_t minimumBitsPerSec,
                             uint64_t maximumBitsPerSec)
    : minimumBitsPerSec_(std::min(minimumBitsPerSec, maximumBitsPerSec)),
      maximumBitsPerSec_(std::max(minimumBitsPerSec, maximumBitsPerSec)),
      targetBitsPerSec_(std::clamp(initialBitsPerSec, minimumBitsPerSec_,
                                   maximumBitsPerSec_)) {}

uint64_t AbrController::observe(const AbrSample& sample) {
  const bool newDrops = haveSample_ && sample.droppedFrames > previousDrops_;
  const bool queueGrowing = haveSample_ && sample.queueDepth > previousQueueDepth_;
  growingSamples_ = queueGrowing ? growingSamples_ + 1 : 0;

  if (newDrops || growingSamples_ >= kGrowthSamplesBeforeBackoff) {
    // Twenty percent is intentionally steep: a queue that is already growing turns
    // latency into seconds unless arrival rate drops below drain rate promptly.
    targetBitsPerSec_ = std::max(minimumBitsPerSec_, percent(targetBitsPerSec_, 80, 100));
    growingSamples_ = 0;
    stableSamples_ = 0;
  } else if (sample.queueDepth == 0 && !newDrops) {
    ++stableSamples_;
    if (stableSamples_ >= kStableSamplesBeforeRecovery) {
      uint64_t ceiling = maximumBitsPerSec_;
      if (sample.throughputBitsPerSec != 0) {
        ceiling = std::max(minimumBitsPerSec_,
                           std::min(ceiling, percent(sample.throughputBitsPerSec, 85, 100)));
      }
      const uint64_t raised = std::max(targetBitsPerSec_ + 1,
                                       percent(targetBitsPerSec_, 105, 100));
      targetBitsPerSec_ = std::clamp(raised, minimumBitsPerSec_, ceiling);
      stableSamples_ = 0;
    }
  } else {
    stableSamples_ = 0;
  }

  previousDrops_ = sample.droppedFrames;
  previousQueueDepth_ = sample.queueDepth;
  haveSample_ = true;
  return targetBitsPerSec_;
}

}  // namespace rcplatform
