#include "rc/annexb.h"

namespace rc::annexb {
namespace {

// H.264, ITU-T H.264 Table 7-1.
constexpr uint8_t kH264Sps = 7;
constexpr uint8_t kH264Pps = 8;
constexpr uint8_t kH264IdrSlice = 5;

// HEVC, ITU-T H.265 Table 7-1.
constexpr uint8_t kHevcVps = 32;
constexpr uint8_t kHevcSps = 33;
constexpr uint8_t kHevcPps = 34;
constexpr uint8_t kHevcBlaWLp = 16;
constexpr uint8_t kHevcCraNut = 21;

// Scanning for 00 00 01 is safe without unescaping: emulation-prevention bytes exist
// precisely so that sequence cannot occur inside a NAL payload.
bool startCodeAt(const uint8_t* data, size_t size, size_t index, size_t& codeLength) {
  if (index + 3 <= size && data[index] == 0 && data[index + 1] == 0 && data[index + 2] == 1) {
    codeLength = 3;
    return true;
  }
  if (index + 4 <= size && data[index] == 0 && data[index + 1] == 0 && data[index + 2] == 0 &&
      data[index + 3] == 1) {
    codeLength = 4;
    return true;
  }
  return false;
}

uint8_t nalTypeOf(Codec codec, uint8_t headerByte) {
  return codec == Codec::H264 ? static_cast<uint8_t>(headerByte & 0x1Fu)
                              : static_cast<uint8_t>((headerByte >> 1) & 0x3Fu);
}

bool isVideoSlice(Codec codec, uint8_t nalType) {
  // H.264 VCL types are 1..5. HEVC reserves the complete 0..31 half of the type
  // space for VCL NAL units.
  return codec == Codec::H264 ? (nalType >= 1 && nalType <= 5) : nalType <= 31;
}

}  // namespace

bool split(const uint8_t* data, size_t size, Codec codec, std::vector<Nal>& out) {
  out.clear();
  if (data == nullptr || size < 4) return false;

  // Find the first start code. Anything before it is not part of any NAL; a stream that
  // does not begin with one is not Annex-B.
  size_t index = 0;
  size_t codeLength = 0;
  while (index < size && !startCodeAt(data, size, index, codeLength)) ++index;
  if (index >= size) return false;

  while (index < size) {
    const size_t nalStart = index + codeLength;
    if (nalStart >= size) break;

    // Walk to the next start code; that byte ends this NAL.
    size_t next = nalStart;
    size_t nextCodeLength = 0;
    while (next < size && !startCodeAt(data, size, next, nextCodeLength)) ++next;

    Nal nal;
    nal.offset = nalStart;
    nal.size = next - nalStart;
    nal.type = nalTypeOf(codec, data[nalStart]);
    // A start code immediately followed by another is malformed but harmless; skip the
    // empty NAL rather than reporting a type read from the next start code's zero byte.
    if (nal.size > 0) out.push_back(nal);

    if (next >= size) break;
    index = next;
    codeLength = nextCodeLength;
  }
  return !out.empty();
}

bool isParameterSet(Codec codec, uint8_t nalType) {
  if (codec == Codec::H264) return nalType == kH264Sps || nalType == kH264Pps;
  return nalType == kHevcVps || nalType == kHevcSps || nalType == kHevcPps;
}

bool isKeyframeSlice(Codec codec, uint8_t nalType) {
  if (codec == Codec::H264) return nalType == kH264IdrSlice;
  // HEVC groups every IRAP picture into 16..21: BLA, IDR and CRA. All of them are
  // random-access points, so all of them are keyframes for our purposes.
  return nalType >= kHevcBlaWLp && nalType <= kHevcCraNut;
}

AccessUnitReport inspect(const uint8_t* data, size_t size, Codec codec) {
  AccessUnitReport report;

  std::vector<Nal> nals;
  report.isAnnexB = split(data, size, codec, nals);
  if (!report.isAnnexB) return report;

  report.nalCount = static_cast<int>(nals.size());

  bool sps = false, pps = false, vps = false;
  bool parameterSetsBeforeFirstKeyframe = false;
  for (const Nal& nal : nals) {
    if (isVideoSlice(codec, nal.type)) report.hasVideoSlice = true;
    if (codec == Codec::H264) {
      if (nal.type == kH264Sps) sps = true;
      if (nal.type == kH264Pps) pps = true;
    } else {
      if (nal.type == kHevcVps) vps = true;
      if (nal.type == kHevcSps) sps = true;
      if (nal.type == kHevcPps) pps = true;
    }
    if (isKeyframeSlice(codec, nal.type)) {
      if (!report.hasKeyframeSlice) {
        parameterSetsBeforeFirstKeyframe =
            codec == Codec::H264 ? (sps && pps) : (vps && sps && pps);
      }
      report.hasKeyframeSlice = true;
    }
  }

  report.hasParameterSets = codec == Codec::H264 ? (sps && pps) : (vps && sps && pps);
  report.isSelfContainedKeyframe =
      report.hasKeyframeSlice && parameterSetsBeforeFirstKeyframe;
  // A non-keyframe carries no obligation: it is decodable if the decoder already has
  // the parameter sets from the keyframe it followed.
  report.decodableFromHere = !report.hasKeyframeSlice || report.isSelfContainedKeyframe;
  return report;
}

AccessUnitReport inspect(const std::vector<uint8_t>& data, Codec codec) {
  return inspect(data.data(), data.size(), codec);
}

}  // namespace rc::annexb
