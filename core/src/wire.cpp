#include "rc/wire.h"

#include <cstring>

namespace rc::wire {
namespace {

// One MiB of consumed prefix, or half the buffer, before compacting. Copying on every
// frame would be O(n^2) over a stream; never copying would grow the buffer without
// bound on a long-lived connection.
constexpr size_t kCompactThreshold = size_t{1024} * 1024;

void appendBigEndian32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(static_cast<uint8_t>(value >> 24));
  out.push_back(static_cast<uint8_t>(value >> 16));
  out.push_back(static_cast<uint8_t>(value >> 8));
  out.push_back(static_cast<uint8_t>(value));
}

void appendBigEndian64(std::vector<uint8_t>& out, uint64_t value) {
  for (int shift = 56; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>((value >> shift) & 0xFFu));
  }
}

uint32_t readBigEndian32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint64_t readBigEndian64(const uint8_t* p) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value = (value << 8) | p[i];
  return value;
}

}  // namespace

const char* errorText(Error error) {
  switch (error) {
    case Error::None: return "none";
    case Error::PayloadTooLarge: return "payload too large";
    case Error::ReservedFlagsSet: return "reserved flag bits set";
    case Error::ReservedHeaderNonZero: return "reserved header bytes non-zero";
  }
  return "unknown";
}

Error encode(uint8_t channel, uint8_t frameFlags, uint64_t ptsMicros, const uint8_t* payload,
             size_t payloadSize, std::vector<uint8_t>& out) {
  if (payloadSize > kMaxPayloadBytes) return Error::PayloadTooLarge;
  if ((frameFlags & ~flags::kKnownMask) != 0) return Error::ReservedFlagsSet;

  out.reserve(out.size() + kHeaderBytes + payloadSize);
  appendBigEndian32(out, static_cast<uint32_t>(payloadSize));
  out.push_back(channel);
  out.push_back(frameFlags);
  out.push_back(0);  // reserved
  out.push_back(0);
  appendBigEndian64(out, ptsMicros);
  if (payloadSize > 0) out.insert(out.end(), payload, payload + payloadSize);
  return Error::None;
}

Error encode(const Frame& frame, std::vector<uint8_t>& out) {
  return encode(frame.channel, frame.flags, frame.ptsMicros, frame.payload.data(),
                frame.payload.size(), out);
}

void Decoder::reset() {
  buffer_.clear();
  readOffset_ = 0;
  failed_ = false;
  failure_ = Error::None;
}

Error Decoder::append(const uint8_t* data, size_t size, std::vector<Frame>& out) {
  // Once framing is lost the stream position is unknown, so there is nothing to
  // resynchronise to. Report the original cause on every subsequent call rather than
  // inventing a new one.
  if (failed_) return failure_;
  if (data != nullptr && size > 0) buffer_.insert(buffer_.end(), data, data + size);

  while (buffer_.size() - readOffset_ >= kHeaderBytes) {
    const uint8_t* header = buffer_.data() + readOffset_;

    // Everything below happens on header arrival, before the body is waited for. See
    // the ordering note in wire.h -- this is the part that must not be reordered.
    const uint32_t payloadLength = readBigEndian32(header);
    if (payloadLength > kMaxPayloadBytes) {
      failed_ = true;
      failure_ = Error::PayloadTooLarge;
      return failure_;
    }
    const uint8_t frameFlags = header[5];
    if ((frameFlags & ~flags::kKnownMask) != 0) {
      failed_ = true;
      failure_ = Error::ReservedFlagsSet;
      return failure_;
    }
    if (header[6] != 0 || header[7] != 0) {
      failed_ = true;
      failure_ = Error::ReservedHeaderNonZero;
      return failure_;
    }

    if (buffer_.size() - readOffset_ - kHeaderBytes < payloadLength) break;

    Frame frame;
    frame.channel = header[4];
    frame.flags = frameFlags;
    frame.ptsMicros = readBigEndian64(header + 8);
    const uint8_t* body = header + kHeaderBytes;
    frame.payload.assign(body, body + payloadLength);
    out.push_back(std::move(frame));

    readOffset_ += kHeaderBytes + payloadLength;
  }

  if (readOffset_ == buffer_.size()) {
    buffer_.clear();
    readOffset_ = 0;
  } else if (readOffset_ >= kCompactThreshold || readOffset_ > buffer_.size() / 2) {
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(readOffset_));
    readOffset_ = 0;
  }
  return Error::None;
}

Error Decoder::append(const std::vector<uint8_t>& data, std::vector<Frame>& out) {
  return append(data.data(), data.size(), out);
}

}  // namespace rc::wire
