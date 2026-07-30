// Tests for Annex-B access-unit inspection.
//
// The property under test is the one protocol.md makes normative: a keyframe must carry
// its own parameter sets. The failure it guards against reproduces only on a *second*
// connection -- the first PC to attach got the parameter sets and decodes happily,
// everyone after it gets a black window -- so it is worth an explicit mechanical check.

#include "rc/annexb.h"

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

using rc::annexb::Codec;

// Builders that mirror what VideoToolbox emits, including its habit of using 4-byte
// start codes for parameter sets and 3-byte ones elsewhere.
void appendNal(std::vector<uint8_t>& out, bool fourByte, uint8_t headerByte,
               const std::vector<uint8_t>& body = {0x00}) {
  if (fourByte) out.push_back(0);
  out.push_back(0);
  out.push_back(0);
  out.push_back(1);
  out.push_back(headerByte);
  out.insert(out.end(), body.begin(), body.end());
}

uint8_t h264Header(uint8_t type) { return static_cast<uint8_t>(0x60u | (type & 0x1Fu)); }
uint8_t hevcHeader(uint8_t type) { return static_cast<uint8_t>((type & 0x3Fu) << 1); }

// ---------------------------------------------------------------------------

void testSplitting() {
  std::printf("NAL splitting\n");

  std::vector<uint8_t> unit;
  appendNal(unit, true, h264Header(7));    // SPS, 4-byte start code
  appendNal(unit, false, h264Header(8));   // PPS, 3-byte
  appendNal(unit, false, h264Header(5), {0xaa, 0xbb, 0xcc});

  std::vector<rc::annexb::Nal> nals;
  check(rc::annexb::split(unit.data(), unit.size(), Codec::H264, nals), "the unit splits");
  check(nals.size() == 3, "three NALs found across mixed start-code lengths");
  if (nals.size() == 3) {
    check(nals[0].type == 7 && nals[1].type == 8 && nals[2].type == 5,
          "H.264 types come from the low 5 bits");
    check(nals[2].size == 4, "the last NAL runs to the end of the buffer");
    check(unit[nals[2].offset] == h264Header(5), "the offset points past the start code");
  }

  // HEVC takes bits 1-6 of the header byte instead, and reading it the H.264 way would
  // silently mistype every NAL in the stream.
  std::vector<uint8_t> hevc;
  appendNal(hevc, true, hevcHeader(32));   // VPS
  appendNal(hevc, true, hevcHeader(33));   // SPS
  appendNal(hevc, true, hevcHeader(34));   // PPS
  appendNal(hevc, false, hevcHeader(19));  // IDR_W_RADL
  nals.clear();
  check(rc::annexb::split(hevc.data(), hevc.size(), Codec::Hevc, nals), "the HEVC unit splits");
  check(nals.size() == 4, "four HEVC NALs");
  if (nals.size() == 4) {
    check(nals[0].type == 32 && nals[1].type == 33 && nals[2].type == 34 && nals[3].type == 19,
          "HEVC types come from bits 1-6");
  }
}

void testNonAnnexBRejected() {
  std::printf("Non-Annex-B payloads\n");

  std::vector<rc::annexb::Nal> nals;
  check(!rc::annexb::split(nullptr, 0, Codec::H264, nals), "a null payload is not Annex-B");

  const std::vector<uint8_t> tiny = {0x00, 0x00};
  check(!rc::annexb::split(tiny.data(), tiny.size(), Codec::H264, nals),
        "a payload too short to hold a start code is not Annex-B");

  // AVCC/length-prefixed is the realistic wrong format, and it must be reported rather
  // than parsed into nonsense -- the fix for it is on the sender.
  const std::vector<uint8_t> avcc = {0x00, 0x00, 0x00, 0x05, 0x65, 0x11, 0x22, 0x33, 0x44};
  check(!rc::annexb::split(avcc.data(), avcc.size(), Codec::H264, nals),
        "a length-prefixed payload is reported as not Annex-B");

  const std::vector<uint8_t> garbage(64, 0xEE);
  check(!rc::annexb::split(garbage.data(), garbage.size(), Codec::H264, nals),
        "random bytes are not Annex-B");
}

void testKeyframeMustCarryParameterSets() {
  std::printf("Keyframes must carry their parameter sets\n");

  // The good case: a self-contained H.264 IDR.
  std::vector<uint8_t> good;
  appendNal(good, true, h264Header(7));
  appendNal(good, true, h264Header(8));
  appendNal(good, false, h264Header(5));
  rc::annexb::AccessUnitReport report = rc::annexb::inspect(good, Codec::H264);
  check(report.isAnnexB && report.nalCount == 3, "the keyframe parses");
  check(report.hasKeyframeSlice, "the IDR slice is recognised");
  check(report.hasParameterSets, "SPS and PPS are both present");
  check(report.decodableFromHere, "so a PC joining here can decode");

  // The bug: an IDR with no parameter sets. Decodes for whoever was already connected,
  // black window for everyone who reconnects.
  std::vector<uint8_t> bare;
  appendNal(bare, false, h264Header(5));
  report = rc::annexb::inspect(bare, Codec::H264);
  check(report.hasKeyframeSlice, "the IDR is still recognised");
  check(!report.hasParameterSets, "the missing parameter sets are noticed");
  check(!report.decodableFromHere, "and the unit is flagged as not decodable from here");

  // Half of them is still wrong.
  std::vector<uint8_t> spsOnly;
  appendNal(spsOnly, true, h264Header(7));
  appendNal(spsOnly, false, h264Header(5));
  check(!rc::annexb::inspect(spsOnly, Codec::H264).decodableFromHere,
        "SPS without PPS is not enough");

  // A non-keyframe carries no such obligation; the decoder already has what it needs.
  std::vector<uint8_t> interFrame;
  appendNal(interFrame, false, h264Header(1));
  report = rc::annexb::inspect(interFrame, Codec::H264);
  check(!report.hasKeyframeSlice, "a non-IDR slice is not a keyframe");
  check(report.decodableFromHere, "and is not required to carry parameter sets");

  // HEVC needs all three, so VPS omitted is a failure that H.264's rule would miss.
  std::vector<uint8_t> hevcGood;
  appendNal(hevcGood, true, hevcHeader(32));
  appendNal(hevcGood, true, hevcHeader(33));
  appendNal(hevcGood, true, hevcHeader(34));
  appendNal(hevcGood, false, hevcHeader(19));
  check(rc::annexb::inspect(hevcGood, Codec::Hevc).decodableFromHere,
        "a complete HEVC keyframe is decodable");

  std::vector<uint8_t> noVps;
  appendNal(noVps, true, hevcHeader(33));
  appendNal(noVps, true, hevcHeader(34));
  appendNal(noVps, false, hevcHeader(20));
  check(!rc::annexb::inspect(noVps, Codec::Hevc).decodableFromHere,
        "HEVC without a VPS is not decodable from here");
}

void testHevcIrapRange() {
  std::printf("HEVC random-access picture types\n");

  // 16..21 are all IRAP pictures -- BLA, IDR and CRA. Treating only IDR as a keyframe
  // would let a CRA-based stream look like it never sends one.
  for (uint8_t type = 16; type <= 21; ++type) {
    check(rc::annexb::isKeyframeSlice(Codec::Hevc, type),
          "HEVC NAL type " + std::to_string(type) + " is a random-access point");
  }
  for (uint8_t type : {uint8_t{0}, uint8_t{1}, uint8_t{15}, uint8_t{22}, uint8_t{32}}) {
    check(!rc::annexb::isKeyframeSlice(Codec::Hevc, type),
          "HEVC NAL type " + std::to_string(type) + " is not");
  }
  check(rc::annexb::isKeyframeSlice(Codec::H264, 5), "H.264 IDR is a keyframe");
  check(!rc::annexb::isKeyframeSlice(Codec::H264, 1), "an H.264 non-IDR slice is not");
}

void testEmulationPreventionIsNotConfusing() {
  std::printf("Emulation prevention does not split a NAL\n");

  // 00 00 03 inside a payload exists so that 00 00 01 cannot occur there. A scanner
  // that unescaped first, or that looked for 00 00 without the 01, would cut the NAL in
  // half here.
  std::vector<uint8_t> unit;
  appendNal(unit, true, h264Header(7), {0x00, 0x00, 0x03, 0x01, 0x42, 0x00, 0x00, 0x03, 0x00});
  appendNal(unit, false, h264Header(5), {0x11, 0x00, 0x00, 0x03, 0x22});

  std::vector<rc::annexb::Nal> nals;
  check(rc::annexb::split(unit.data(), unit.size(), Codec::H264, nals), "the unit splits");
  check(nals.size() == 2, "escaped bytes do not create extra NALs");
  if (nals.size() == 2) {
    check(nals[0].type == 7 && nals[1].type == 5, "both NALs keep their types");
  }

  // A trailing zero before the next start code belongs to the previous NAL's trailing
  // bytes; splitting must still find exactly two.
  std::vector<uint8_t> padded;
  appendNal(padded, true, h264Header(8), {0x00});
  appendNal(padded, true, h264Header(5), {0x33});
  nals.clear();
  rc::annexb::split(padded.data(), padded.size(), Codec::H264, nals);
  check(nals.size() == 2, "a 4-byte start code after a zero-terminated NAL is found once");
}

void testLeadingGarbageAndEmptyNals() {
  std::printf("Malformed but survivable input\n");

  // Bytes before the first start code are skipped rather than treated as a NAL.
  std::vector<uint8_t> leading = {0xde, 0xad, 0xbe, 0xef};
  appendNal(leading, true, h264Header(5));
  std::vector<rc::annexb::Nal> nals;
  check(rc::annexb::split(leading.data(), leading.size(), Codec::H264, nals),
        "leading garbage does not defeat the parser");
  check(nals.size() == 1 && nals[0].type == 5, "only the real NAL is reported");

  // Back-to-back start codes produce an empty NAL, which must not be reported with a
  // type read from the following start code's zero byte.
  const std::vector<uint8_t> doubled = {0, 0, 0, 1, 0, 0, 0, 1, h264Header(5), 0x00};
  nals.clear();
  check(rc::annexb::split(doubled.data(), doubled.size(), Codec::H264, nals),
        "back-to-back start codes still parse");
  for (const rc::annexb::Nal& nal : nals) check(nal.size > 0, "no empty NAL is reported");

  // A start code at the very end with nothing after it.
  const std::vector<uint8_t> dangling = {0, 0, 0, 1};
  nals.clear();
  check(!rc::annexb::split(dangling.data(), dangling.size(), Codec::H264, nals),
        "a start code with no NAL behind it yields nothing");
}

}  // namespace

int main() {
  testSplitting();
  testNonAnnexBRejected();
  testKeyframeMustCarryParameterSets();
  testHevcIrapRange();
  testEmulationPreventionIsNotConfusing();
  testLeadingGarbageAndEmptyNals();

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
