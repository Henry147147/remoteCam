// Where the pixels come from.
//
// One decision point, isolated here so the rest of the DLL never has to care: either a
// producer in the user's session is publishing frames into the shared-memory ring, or
// nobody is and we synthesise a placeholder.
//
// PLAN.md's "always produce frames" rule is absolute. Several consumers in the
// compatibility matrix drop the device or surface an error when a camera stops
// delivering, so there is no state in which returning nothing is acceptable -- not
// while waiting for a phone, not while the ring is being created, not during a
// producer restart.

#ifndef RC_VCAM_FRAME_SOURCE_H
#define RC_VCAM_FRAME_SOURCE_H

#include <cstdint>

#include "rcwin/nv12.h"
#include "rcwin/shm_ring.h"

namespace rcvcam {

class FrameSource {
 public:
  // Creates the Global\ ring. This is the privileged half of the handoff and it works
  // here precisely because this code runs inside the Frame Server as LOCAL SERVICE; the
  // same call from the user's session fails with ERROR_ACCESS_DENIED. A failure is
  // logged and swallowed -- we degrade to placeholder-only rather than refusing to be
  // a camera at all, which would turn a frame-delivery problem into "RemoteCam does not
  // appear in Zoom" and send the diagnosis in entirely the wrong direction.
  void start();
  void stop();

  // Fills `dst` with one frame. Returns true when the content came from the ring,
  // false when it is the placeholder -- the caller only uses this for logging.
  bool fill(uint8_t* dst, const rcwin::Nv12Layout& layout, uint64_t frameIndex);

 private:
  rcwin::FrameRing ring_;
  bool ringReady_ = false;

  // Tracks live/placeholder transitions so the log records each change once instead of
  // thirty times a second.
  bool lastWasLive_ = false;
  bool everLogged_ = false;
  bool mismatchLogged_ = false;

  // What we last told the producer we want. Media Foundation hands us the layout per
  // frame, so this is where the negotiated geometry is actually known; publishing it
  // once per change keeps the ring write off the per-frame path.
  int requestedWidth_ = 0;
  int requestedHeight_ = 0;
};

// How long a published frame stays usable before we fall back to the placeholder.
// 250 ms is eight frames at 30 fps: long enough to ride out a scheduling hiccup or a
// single dropped network packet, short enough that a user who closes the phone app
// sees the placeholder rather than a frozen image of themselves.
inline constexpr uint64_t kStaleAfterMillis = 250;

}  // namespace rcvcam

#endif  // RC_VCAM_FRAME_SOURCE_H
