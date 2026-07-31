// RemoteCam — Annex-B access-unit inspection.
//
// Channel 1 carries one access unit per message as an Annex-B byte stream
// (docs/protocol.md "Video"). This file splits it into NAL units and answers the one
// question the receiver actually has to ask before handing bytes to a decoder:
//
//   does every keyframe carry its own parameter sets?
//
// protocol.md requires VPS/SPS/PPS to be prepended to EVERY keyframe rather than sent
// once, because a PC that joins or resets mid-stream has no side channel to fetch them
// from. A stream that violates that decodes fine for whoever was connected at the start
// and produces a permanently black window for anyone who reconnects -- a bug that
// reproduces only on the second connection, which is exactly the kind worth catching
// mechanically rather than by eye.
//
// Deliberately no bitstream parsing beyond the NAL header byte. Reading SPS fields
// would mean an Exp-Golomb reader and emulation-prevention unescaping, which is a
// decoder's job; this is a structural check.
//
// No platform APIs, so this builds and is tested on the Linux and macOS runners
// alongside the rest of core/.

#ifndef RC_ANNEXB_H
#define RC_ANNEXB_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rc::annexb {

enum class Codec { H264, Hevc };

struct Nal {
  // Offset of the NAL's first byte, i.e. just past the start code. The start code
  // itself is not part of the NAL.
  size_t offset = 0;
  size_t size = 0;
  // Codec-specific type: H.264 takes the low 5 bits of byte 0, HEVC bits 1-6.
  uint8_t type = 0;
};

// Splits an access unit at 3- and 4-byte start codes. Both lengths occur in the same
// stream -- VideoToolbox emits 4-byte codes before parameter sets and 3-byte codes
// elsewhere -- so handling only one is a bug that hides until a particular encoder
// setting changes.
//
// Returns false when there is no start code at all, which means the payload is not
// Annex-B (most likely it is AVCC/length-prefixed, and the fix is on the sender).
bool split(const uint8_t* data, size_t size, Codec codec, std::vector<Nal>& out);

bool isParameterSet(Codec codec, uint8_t nalType);
bool isKeyframeSlice(Codec codec, uint8_t nalType);

struct AccessUnitReport {
  bool isAnnexB = false;
  int nalCount = 0;
  bool hasParameterSets = false;   // H.264: SPS and PPS. HEVC: VPS, SPS and PPS.
  bool hasVideoSlice = false;
  bool hasKeyframeSlice = false;
  // True only when the complete parameter-set family appears before the first
  // random-access slice. Sets appended after a slice cannot initialise a decoder in
  // time for that slice and therefore do not make the access unit self-contained.
  bool isSelfContainedKeyframe = false;
  // The property protocol.md actually requires. False for a keyframe whose parameter
  // sets are missing, true for a non-keyframe regardless.
  bool decodableFromHere = false;
};

AccessUnitReport inspect(const uint8_t* data, size_t size, Codec codec);
AccessUnitReport inspect(const std::vector<uint8_t>& data, Codec codec);

}  // namespace rc::annexb

#endif  // RC_ANNEXB_H
