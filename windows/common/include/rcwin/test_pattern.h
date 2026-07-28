// The frames RemoteCam produces when there is no phone.
//
// This pattern is designed for the probe, not for looks. It is split into two regions
// with opposite guarantees:
//
//   static region  -- byte-identical on every frame. Hashing it catches a wrong
//                     stride, a wrong plane offset, or a colour range mistake, all of
//                     which survive a visual "looks fine to me" check.
//   moving region  -- guaranteed different on every frame. Hashing it distinguishes
//                     "the camera is delivering frames" from "the camera delivered one
//                     frame and stalled", which look identical in a preview window.
//
// rc-vcam-probe asserts both properties, so a single 60-frame capture proves
// correctness and liveness independently.

#ifndef RCWIN_TEST_PATTERN_H
#define RCWIN_TEST_PATTERN_H

#include <cstdint>

#include "rcwin/nv12.h"

namespace rcwin {

enum class PatternStyle {
  // Emitted by rc-vcam.dll when no producer is writing frames. PLAN.md's "always
  // produce frames" rule: several consumers drop or error on a stalled device, so
  // there is never a case where we send nothing.
  Placeholder,
  // Emitted by rc-fakewriter through the shared-memory ring. Deliberately a different
  // palette and label from Placeholder -- when the two are told apart at a glance in
  // the Windows Camera app, that *is* the proof that the Session 0 handoff works.
  Writer,
};

// Row ranges of the two regions, in luma pixels.
struct PatternRegions {
  int staticTop = 0;
  int staticHeight = 0;
  int movingTop = 0;
  int movingHeight = 0;
};

PatternRegions patternRegions(const Nv12Layout& layout);

// Deterministic in (layout, frameIndex, style): the same arguments always produce the
// same bytes. That is what lets the probe compare hashes across a capture at all.
void renderPattern(uint8_t* dst, const Nv12Layout& layout, uint64_t frameIndex,
                   PatternStyle style);

}  // namespace rcwin

#endif  // RCWIN_TEST_PATTERN_H
