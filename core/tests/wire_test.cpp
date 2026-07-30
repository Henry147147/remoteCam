// Tests for the 16-byte framing and the streaming reassembler.
//
// The header vector and the chunked-delivery sweep both come from
// ios/RemoteCamTests/WireFrameTests.swift. The chunk sweep is the one that matters
// most: TCP splits wherever it likes, and a decoder that only ever sees whole frames in
// testing will meet its first straddled header in production.

#include "rc/wire.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("  FAIL: %s\n", what.c_str());
  }
}

std::string hex(const std::vector<uint8_t>& bytes) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (uint8_t b : bytes) {
    out.push_back(digits[b >> 4]);
    out.push_back(digits[b & 0x0F]);
    out.push_back(' ');
  }
  if (!out.empty()) out.pop_back();
  return out;
}

void checkBytes(const std::vector<uint8_t>& got, const std::vector<uint8_t>& want,
                const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failures;
    std::printf("  FAIL: %s\n    got  %s\n    want %s\n", what.c_str(), hex(got).c_str(),
                hex(want).c_str());
  }
}

void checkError(rc::wire::Error got, rc::wire::Error want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failures;
    std::printf("  FAIL: %s (got %s, want %s)\n", what.c_str(), rc::wire::errorText(got),
                rc::wire::errorText(want));
  }
}

using rc::wire::Frame;

// ---------------------------------------------------------------------------

void testHeaderLayout() {
  std::printf("Header layout\n");

  // Verbatim from WireFrameTests.testEncodesHeaderExactly: stats channel, encrypted
  // flag, pts 0x0102030405060708, two payload bytes.
  std::vector<uint8_t> encoded;
  checkError(rc::wire::encode(3, rc::wire::flags::kEncrypted, 0x0102030405060708ull,
                              std::vector<uint8_t>{0xaa, 0xbb}.data(), 2, encoded),
             rc::wire::Error::None, "the frame encodes");
  checkBytes(std::vector<uint8_t>(encoded.begin(), encoded.begin() + 16),
             {0, 0, 0, 2, 3, 2, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8},
             "the 16-byte header matches the Swift encoding exactly");
  checkBytes(std::vector<uint8_t>(encoded.begin() + 16, encoded.end()), {0xaa, 0xbb},
             "the payload follows the header verbatim");

  // A zero-length payload is legal and appears in the Swift reassembly test.
  std::vector<uint8_t> empty;
  checkError(rc::wire::encode(2, 0, 0, nullptr, 0, empty), rc::wire::Error::None,
             "an empty payload encodes");
  check(empty.size() == 16, "an empty frame is header-only");
}

void testEncodeRejections() {
  std::printf("Encode rejections\n");

  std::vector<uint8_t> out;
  const std::vector<uint8_t> payload(4, 0);
  // Bits 3-7 are reserved. Emitting one would make it unusable for a future version,
  // because peers would already be seeing it set.
  checkError(rc::wire::encode(0, 0x08, 0, payload.data(), payload.size(), out),
             rc::wire::Error::ReservedFlagsSet, "a reserved flag bit is refused");
  checkError(rc::wire::encode(0, 0x80, 0, payload.data(), payload.size(), out),
             rc::wire::Error::ReservedFlagsSet, "the top flag bit is refused");
  checkError(rc::wire::encode(0, rc::wire::flags::kKnownMask, 0, payload.data(),
                              payload.size(), out),
             rc::wire::Error::None, "all three known flags together are fine");
}

void testStreamingReassembly() {
  std::printf("Streaming reassembly\n");

  // Three frames, mirroring the Swift fixture: a small control frame, a large video
  // keyframe, and an empty audio frame.
  std::vector<uint8_t> stream;
  rc::wire::encode(0, 0, 123, std::vector<uint8_t>{1, 2, 3}.data(), 3, stream);
  const std::vector<uint8_t> big(2049, 0xa5);
  rc::wire::encode(1, rc::wire::flags::kKeyframe, 9876543, big.data(), big.size(), stream);
  rc::wire::encode(2, 0, 0, nullptr, 0, stream);

  // Feeding the same stream at many chunk sizes is what exercises headers split across
  // reads. Size 1 is the pathological case and the most valuable one.
  for (size_t chunk : {size_t{1}, size_t{7}, size_t{13}, size_t{16}, size_t{17}, size_t{256},
                       size_t{1024}, stream.size()}) {
    rc::wire::Decoder decoder;
    std::vector<Frame> frames;
    rc::wire::Error err = rc::wire::Error::None;
    for (size_t offset = 0; offset < stream.size() && err == rc::wire::Error::None;
         offset += chunk) {
      const size_t take = (offset + chunk <= stream.size()) ? chunk : stream.size() - offset;
      err = decoder.append(stream.data() + offset, take, frames);
    }
    const std::string suffix = " at chunk size " + std::to_string(chunk);
    checkError(err, rc::wire::Error::None, "no framing error" + suffix);
    check(frames.size() == 3, "all three frames arrive" + suffix);
    if (frames.size() != 3) continue;

    check(frames[0].channel == 0 && frames[0].ptsMicros == 123 &&
              frames[0].payload == std::vector<uint8_t>({1, 2, 3}),
          "control frame is intact" + suffix);
    check(frames[1].channel == 1 && frames[1].isKeyframe() &&
              frames[1].ptsMicros == 9876543 && frames[1].payload == big,
          "video keyframe is intact" + suffix);
    check(frames[2].channel == 2 && frames[2].payload.empty(),
          "empty audio frame is intact" + suffix);
    check(decoder.buffered() == 0, "nothing is left buffered" + suffix);
  }
}

void testValidatesBeforeBufferingBody() {
  std::printf("Header validation precedes body buffering\n");

  // From WireFrameTests.testRejectsOversizedPayloadBeforeBufferingBody. Only the header
  // is supplied -- no body at all -- and the decoder must still reject it. A decoder
  // that waited for the body would sit holding 16 MiB for a peer it has already
  // decided to disconnect.
  const uint32_t advertised = rc::wire::kMaxPayloadBytes + 1;
  std::vector<uint8_t> header = {
      static_cast<uint8_t>(advertised >> 24), static_cast<uint8_t>(advertised >> 16),
      static_cast<uint8_t>(advertised >> 8),  static_cast<uint8_t>(advertised),
      1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

  rc::wire::Decoder decoder;
  std::vector<Frame> frames;
  checkError(decoder.append(header, frames), rc::wire::Error::PayloadTooLarge,
             "an oversized length is refused from the header alone");
  check(frames.empty(), "no frame is produced");
  check(decoder.failed(), "the decoder is permanently failed");
  checkError(decoder.append(header, frames), rc::wire::Error::PayloadTooLarge,
             "the original cause is reported on every later call");

  // Exactly at the cap is legal; the bound is inclusive. Checked on the header alone so
  // the test does not have to materialise 16 MiB.
  std::vector<uint8_t> atCap = {
      static_cast<uint8_t>(rc::wire::kMaxPayloadBytes >> 24),
      static_cast<uint8_t>(rc::wire::kMaxPayloadBytes >> 16),
      static_cast<uint8_t>(rc::wire::kMaxPayloadBytes >> 8),
      static_cast<uint8_t>(rc::wire::kMaxPayloadBytes),
      1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  rc::wire::Decoder atCapDecoder;
  checkError(atCapDecoder.append(atCap, frames), rc::wire::Error::None,
             "a payload exactly at the cap is accepted");
  check(!atCapDecoder.failed(), "and the decoder waits for its body");

  // Reserved flag bits and reserved header bytes are rejected the same way.
  rc::wire::Decoder flagDecoder;
  checkError(flagDecoder.append({0, 0, 0, 0, 0, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}, frames),
             rc::wire::Error::ReservedFlagsSet, "a reserved flag bit is refused on receive");

  rc::wire::Decoder reservedDecoder;
  checkError(reservedDecoder.append({0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0}, frames),
             rc::wire::Error::ReservedHeaderNonZero,
             "a non-zero reserved u16 is refused on receive");
}

void testUnknownChannelsSurvive() {
  std::printf("Unknown channels reach the caller\n");

  // Framing does not police channel numbers. Channel 2 must be ignored rather than
  // errored per protocol.md, and an unknown channel likewise -- both decisions belong
  // to the session layer, which cannot make them if framing has already hung up.
  std::vector<uint8_t> stream;
  rc::wire::encode(2, 0, 0, std::vector<uint8_t>{9}.data(), 1, stream);
  rc::wire::encode(200, 0, 0, std::vector<uint8_t>{9}.data(), 1, stream);

  rc::wire::Decoder decoder;
  std::vector<Frame> frames;
  checkError(decoder.append(stream, frames), rc::wire::Error::None,
             "reserved and unknown channels are not framing errors");
  check(frames.size() == 2, "both frames are delivered");
  if (frames.size() == 2) {
    check(frames[0].channel == 2, "the reserved audio channel is passed through");
    check(frames[1].channel == 200, "an unknown channel is passed through");
  }
}

void testBufferDoesNotGrowUnbounded() {
  std::printf("Buffer compaction over a long stream\n");

  // A connection lives for hours. Without compaction the consumed prefix would be
  // retained for the whole session; with compaction on every frame the copying would be
  // quadratic. Assert the buffer stays small over many frames.
  std::vector<uint8_t> one;
  const std::vector<uint8_t> payload(64, 0x5a);
  rc::wire::encode(1, 0, 0, payload.data(), payload.size(), one);

  rc::wire::Decoder decoder;
  size_t highWater = 0;
  for (int i = 0; i < 20000; ++i) {
    std::vector<Frame> frames;
    decoder.append(one, frames);
    check(frames.size() == 1 || g_failures > 0, "one frame per append");
    if (frames.size() != 1) break;
    if (decoder.buffered() > highWater) highWater = decoder.buffered();
  }
  check(highWater == 0, "a fully consumed stream leaves nothing buffered");

  // Half a header at a time: the decoder must hold the partial header and never
  // mistake it for a frame.
  rc::wire::Decoder partial;
  std::vector<Frame> frames;
  partial.append(one.data(), 8, frames);
  check(frames.empty(), "half a header yields no frame");
  check(partial.buffered() == 8, "the partial header is retained");
  partial.append(one.data() + 8, one.size() - 8, frames);
  check(frames.size() == 1, "the frame completes when the rest arrives");
  check(partial.buffered() == 0, "and the buffer drains");
}

}  // namespace

int main() {
  testHeaderLayout();
  testEncodeRejections();
  testStreamingReassembly();
  testValidatesBeforeBufferingBody();
  testUnknownChannelsSurvive();
  testBufferDoesNotGrowUnbounded();

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
