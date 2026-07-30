// The Session 0 frame handoff.
//
// rc-vcam.dll is loaded by the Media Foundation Frame Server -- svchost.exe -k Camera,
// running as LOCAL SERVICE in Session 0 -- while the process that produces frames runs
// in the user's interactive session. Frames therefore have to cross a session
// boundary, and this ring is how.
//
// WHO CREATES IT, AND WHY IT IS NOT THE OBVIOUS WAY ROUND
//
// The original design had the frame producer create the section. It cannot. Creating a
// section in the Global\ namespace requires SeCreateGlobalPrivilege, which a
// non-elevated interactive process does not hold -- CreateFileMapping fails with
// ERROR_ACCESS_DENIED. Elevating a consumer webcam app to work around that is not an
// acceptable trade.
//
// Opening a Global\ object needs no privilege, only DACL permission. So the roles are
// inverted: rc-vcam.dll, which runs as LOCAL SERVICE and *does* hold the privilege,
// calls create(); the producer calls open(). The consequence is that the ring only
// exists while some application has the camera open, which is fine -- with no consumer
// there is nobody to send frames to.
//
// CONCURRENCY
//
// A seqlock per slot, so the writer never waits on a reader. A non-blocking named
// mutex serialises the write with the rare consumer-generation transition and also
// prevents two unsupported producers from corrupting the sequence counters. The
// writer marks a slot odd, fills it, marks it even; a reader that sees an odd sequence
// or a sequence that moved under it discards the read and retries against whatever is
// current. Dropping a frame is always better than stalling the Frame Server.

#ifndef RCWIN_SHM_RING_H
#define RCWIN_SHM_RING_H

#include <windows.h>

#include <cstdint>

namespace rcwin {

inline constexpr uint32_t kRingMagic = 0x4D414352u;    // "RCAM"
// 3 adds the producer claim and the consumer's requested geometry. Both changed the
// header layout, so a v2 producer and a v3 consumer must not talk to each other --
// open() and create() reject a version they do not recognise. They ship in one
// installer, so the break is contained to a half-upgraded developer tree.
inline constexpr uint32_t kRingVersion = 3u;
inline constexpr uint32_t kRingSlots = 4u;
inline constexpr uint32_t kFourccNv12 = 0x3231564Eu;   // "NV12"

// Slots are sized for 3840x2160 NV12 regardless of the format in use -- about 50 MB of
// pagefile-backed commit for the ring as a whole. That is cheap, and it buys something
// worth more than the memory: a resolution change becomes a formatGeneration bump
// rather than a re-created section that every already-attached reader would have to
// notice and reopen.
inline constexpr uint32_t kRingMaxWidth = 3840u;
inline constexpr uint32_t kRingMaxHeight = 2160u;
inline constexpr uint32_t kRingSlotBytes = kRingMaxWidth * kRingMaxHeight * 3u / 2u;

struct FrameInfo {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
  uint32_t format = kFourccNv12;
  uint64_t ptsMicros = 0;
  uint64_t writeSeq = 0;
  uint32_t bytesUsed = 0;
};

struct RingNames {
  const wchar_t* section;
  const wchar_t* event;
  const wchar_t* writeGuard;
};

// The real objects, in the Global\ namespace, as described above.
RingNames globalRingNames();

// Local\ equivalents, for tests only.
//
// The seqlock is the subtle part of this file and it is worth testing directly, but
// exercising it through the production names would require SeCreateGlobalPrivilege and
// therefore an elevated test runner -- which would mean the test never runs in practice.
// Local\ names are per-session, so a single-process test can create both ends.
RingNames testRingNames();

class FrameRing {
 public:
  FrameRing() = default;
  ~FrameRing();

  FrameRing(const FrameRing&) = delete;
  FrameRing& operator=(const FrameRing&) = delete;

  // Creates the section and event, or attaches to them if they already exist.
  // Requires SeCreateGlobalPrivilege, so this only succeeds from the Frame Server or
  // from an elevated process. The DACL grants the interactive user read+write so an
  // unelevated producer can push frames, and ALL APPLICATION PACKAGES read so that
  // packaged consumers such as the Windows Camera app can be served.
  //
  // Security note, stated plainly because it is a real trade and not an oversight:
  // this lets any interactive user on the machine write frames into the camera. For a
  // single-user desktop that is the intent. On a shared or multi-session machine it
  // would need tightening to the specific user who owns the RemoteCam session.
  HRESULT create(RingNames names = globalRingNames());

  // Opens an existing ring. No privilege required -- only that the DACL above permits
  // it. Returns HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) when no consumer has the
  // camera open, which is an ordinary state, not an error worth alarming about.
  HRESULT open(bool writable, RingNames names = globalRingNames());

  void close();

  bool valid() const { return view_ != nullptr; }

  // Declares the geometry this consumer wants published, so a producer can adapt
  // instead of having its frames silently discarded for not matching.
  //
  // The published width/height/stride in the header are written by the *producer* and
  // describe what it last sent. That is the wrong direction for negotiation: the
  // consumer is the one that picked a media type. These two calls are the other
  // direction. Consumer-only; a producer calling this is a bug.
  HRESULT requestGeometry(uint32_t width, uint32_t height, uint32_t format);

  //   S_OK      a consumer has declared what it wants
  //   S_FALSE   none has yet, so publish whatever you like and expect it to be dropped
  //             if it does not happen to match
  HRESULT requestedGeometry(uint32_t& width, uint32_t& height, uint32_t& format) const;

  // Publishes a frame. Never blocks. `bytes` must not exceed kRingSlotBytes.
  //
  // EXACTLY ONE LOGICAL PRODUCER, and open(writable) now enforces it rather than
  // trusting callers to. The first writer to open claims the ring by recording its
  // process id and process start time; a second writer's open fails with
  // ERROR_ALREADY_EXISTS instead of interleaving unrelated frames into the same slot.
  // The claim is released on close(), and reclaimed automatically when the recorded
  // process is gone -- start time is recorded alongside the id because pids are
  // recycled, and inheriting a stranger's claim would wedge the ring until reboot.
  //
  // A caller that cannot open a process to test it (a producer in another user's
  // session) treats the claim as live rather than stealing it.
  //
  // Geometry in `info` is recorded as given and NOT validated here. That asymmetry is
  // deliberate: a hostile producer would map the section and write it directly rather
  // than call this, so validating on the write side would buy nothing. The check that
  // matters is on the read side, below.
  HRESULT writeFrame(const uint8_t* src, uint32_t bytes, const FrameInfo& info);

  // Copies the most recent complete frame into `dst`.
  //   S_OK      a frame was copied and `info` describes it
  //   S_FALSE   nothing published yet, or the published frame's geometry is not
  //             self-consistent and was rejected
  // A torn read is retried internally; only a persistently contended slot fails, which
  // in practice means the writer is producing faster than memcpy can keep up.
  //
  // On S_OK the caller may rely on `info` being internally consistent: even dimensions
  // within the ring's limits, stride >= width, and stride*height*3/2 <= bytesUsed <=
  // kRingSlotBytes. Consumers index their buffers with these values, so they are
  // checked here rather than trusted -- the section is writable by any interactive user.
  HRESULT readLatest(uint8_t* dst, uint32_t dstCapacity, FrameInfo& info);

  // Milliseconds since the last publish, or UINT64_MAX if there has never been one.
  // Used to decide when to fall back to the placeholder pattern.
  uint64_t millisSinceLastWrite() const;

  // Signalled on every publish. A reader may wait on this instead of polling; it is an
  // auto-reset event, so exactly one waiter is released per frame.
  HANDLE frameEvent() const { return event_; }

 private:
  HRESULT mapView(bool writable);

  HANDLE section_ = nullptr;
  HANDLE event_ = nullptr;
  HANDLE writeGuard_ = nullptr;
  void* view_ = nullptr;
  bool owner_ = false;
  // True when this instance holds the single-producer claim, so close() knows to
  // release it and does not clear a claim belonging to somebody else.
  bool producerClaim_ = false;
  uint32_t consumerGeneration_ = 0;
  // Edge-triggers the "implausible geometry" warning so a producer stuck publishing bad
  // frames logs once rather than at frame rate.
  bool rejectedLogged_ = false;
};

}  // namespace rcwin

#endif  // RCWIN_SHM_RING_H
