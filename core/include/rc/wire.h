// RemoteCam — the 16-byte frame header and the streaming decoder that reassembles it.
//
// Normative source is docs/protocol.md "Framing"; the shipping counterpart is
// ios/RemoteCam/Sources/Wire/WireFrame.swift. This is the layer directly exposed to the
// network, so everything it does with a length is done before the length is trusted.
//
//  0               1               2               3
// +---------------------------------------------------------------+
// |                        length (u32, BE)                       |  payload bytes only
// +---------------+---------------+-------------------------------+
// |  channel (u8) |   flags (u8)  |        reserved (u16)         |
// +---------------+---------------+-------------------------------+
// |                     pts_micros (u64, BE)                      |
// +---------------------------------------------------------------+
//
// ONE ORDERING DETAIL IS OBSERVABLE AND MUST NOT BE "TIDIED"
//
// The decoder validates length, flags and reserved as soon as the 16 header bytes have
// arrived -- before it waits for the body. Deferring those checks until the frame is
// complete would mean buffering up to 16 MiB on behalf of a peer we have already
// decided to hang up on. The Swift suite asserts this ordering
// (testRejectsOversizedPayloadBeforeBufferingBody), so it is part of the contract
// rather than an implementation detail.
//
// A framing error is fatal to the connection: the stream position is no longer known,
// so there is nothing to resynchronise to. That is a deliberate contrast with a
// malformed *control message*, which only costs that one message.

#ifndef RC_WIRE_H
#define RC_WIRE_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rc::wire {

inline constexpr size_t kHeaderBytes = 16;

// 16 MiB. A frame larger than this is a bug or an attack; protocol.md says close the
// connection rather than try to carry it. The bound is inclusive.
inline constexpr uint32_t kMaxPayloadBytes = 16u * 1024u * 1024u;

enum class Channel : uint8_t {
  Control = 0,
  Video = 1,
  // Reserved for v1 and deliberately not an error: receivers must ignore it so adding
  // audio later is a capability flag rather than a version bump.
  Audio = 2,
  Stats = 3,
};

namespace flags {
inline constexpr uint8_t kKeyframe = 1u << 0;
inline constexpr uint8_t kEncrypted = 1u << 1;
inline constexpr uint8_t kEndOfFragment = 1u << 2;
// Bits 3-7 are reserved and must be zero. Accepting them would make them unusable
// later, because some peer would already be setting them.
inline constexpr uint8_t kKnownMask = kKeyframe | kEncrypted | kEndOfFragment;
}  // namespace flags

enum class Error {
  None = 0,
  PayloadTooLarge,
  ReservedFlagsSet,
  ReservedHeaderNonZero,
};

const char* errorText(Error error);

struct Frame {
  // Kept as a raw byte rather than the enum: an unknown channel is data to be dropped
  // by a higher layer, not a framing error, and casting it here would lose that.
  uint8_t channel = 0;
  uint8_t flags = 0;
  uint64_t ptsMicros = 0;
  std::vector<uint8_t> payload;

  bool isKeyframe() const { return (flags & flags::kKeyframe) != 0; }
};

// Appends header+payload to `out`. Fails on an oversized payload or a reserved flag bit
// rather than emitting bytes the other side is required to hang up on.
Error encode(uint8_t channel, uint8_t frameFlags, uint64_t ptsMicros,
             const uint8_t* payload, size_t payloadSize, std::vector<uint8_t>& out);
Error encode(const Frame& frame, std::vector<uint8_t>& out);

// Reassembles frames from a TCP byte stream, which delivers arbitrary chunks with no
// relationship to message boundaries.
class Decoder {
 public:
  // Feeds `size` bytes and appends every frame that completed to `out`.
  //
  // On error the decoder is left permanently failed: `failed()` stays true and further
  // append() calls do nothing. Frames completed earlier in the same call are still
  // appended, so a caller that wants to process them before closing can. Swift discards
  // them instead; either is fine because the connection is going away, and keeping them
  // makes the failure easier to diagnose.
  Error append(const uint8_t* data, size_t size, std::vector<Frame>& out);
  Error append(const std::vector<uint8_t>& data, std::vector<Frame>& out);

  bool failed() const { return failed_; }
  // Bytes held pending completion of the current frame. Exposed for tests and for a
  // caller that wants to alarm on a peer that stalls mid-frame.
  size_t buffered() const { return buffer_.size() - readOffset_; }
  void reset();

 private:
  std::vector<uint8_t> buffer_;
  size_t readOffset_ = 0;
  bool failed_ = false;
  Error failure_ = Error::None;
};

}  // namespace rc::wire

#endif  // RC_WIRE_H
