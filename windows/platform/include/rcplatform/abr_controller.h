#ifndef RCPLATFORM_ABR_CONTROLLER_H
#define RCPLATFORM_ABR_CONTROLLER_H

#include <cstdint>

namespace rcplatform {

struct AbrSample {
  uint32_t queueDepth = 0;
  uint64_t droppedFrames = 0;       // cumulative for the connection
  uint64_t throughputBitsPerSec = 0;
};

// Receiver-owned adaptive bitrate controller. Samples arrive at about 2 Hz. Queue
// growth or new drops cuts the target quickly; an empty stable queue recovers only
// after five seconds and in five-percent steps. The phone applies the returned target
// but never raises it on its own, so this object remains the single source of truth.
class AbrController {
 public:
  AbrController(uint64_t initialBitsPerSec, uint64_t minimumBitsPerSec,
                uint64_t maximumBitsPerSec);

  uint64_t observe(const AbrSample& sample);
  uint64_t targetBitsPerSec() const { return targetBitsPerSec_; }

 private:
  uint64_t minimumBitsPerSec_ = 0;
  uint64_t maximumBitsPerSec_ = 0;
  uint64_t targetBitsPerSec_ = 0;
  uint64_t previousDrops_ = 0;
  uint32_t previousQueueDepth_ = 0;
  uint32_t growingSamples_ = 0;
  uint32_t stableSamples_ = 0;
  bool haveSample_ = false;
};

}  // namespace rcplatform

#endif  // RCPLATFORM_ABR_CONTROLLER_H
