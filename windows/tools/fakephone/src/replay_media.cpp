#include "rcfakephone/replay_media.h"

#include <fstream>
#include <iterator>
#include <utility>

#include "rc/annexb.h"
#include "rc/wire.h"

namespace rcfakephone {
namespace {

struct StartCode {
  size_t offset = 0;
  size_t nalOffset = 0;
};

std::vector<StartCode> startCodes(const std::vector<uint8_t>& bytes) {
  std::vector<StartCode> result;
  size_t index = 0;
  while (index + 3 <= bytes.size()) {
    if (bytes[index] == 0 && bytes[index + 1] == 0 && bytes[index + 2] == 1) {
      result.push_back({index, index + 3});
      index += 3;
    } else if (index + 4 <= bytes.size() && bytes[index] == 0 && bytes[index + 1] == 0 &&
               bytes[index + 2] == 0 && bytes[index + 3] == 1) {
      result.push_back({index, index + 4});
      index += 4;
    } else {
      ++index;
    }
  }
  return result;
}

bool isAud(const std::vector<uint8_t>& bytes, const StartCode& code,
           rc::control::Codec codec) {
  if (code.nalOffset >= bytes.size()) return false;
  if (codec == rc::control::Codec::H264) return (bytes[code.nalOffset] & 0x1f) == 9;
  return ((bytes[code.nalOffset] >> 1) & 0x3f) == 35;
}

}  // namespace

bool ReplayMedia::load(const std::filesystem::path& path, rc::control::Codec codec,
                       uint32_t fps, std::string& reason) {
  units_.clear();
  cursor_ = 0;
  cycle_ = 0;
  durationMicros_ = 0;
  std::ifstream input(path, std::ios::in | std::ios::binary);
  if (!input.is_open()) {
    reason = "cannot open replay file";
    return false;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                             std::istreambuf_iterator<char>());
  if (bytes.empty()) {
    reason = "replay file is empty";
    return false;
  }

  const rc::annexb::Codec annexbCodec = codec == rc::control::Codec::Hevc
                                            ? rc::annexb::Codec::Hevc
                                            : rc::annexb::Codec::H264;
  if (!rc::annexb::inspect(bytes, annexbCodec).isAnnexB) {
    reason = "replay file is not Annex-B";
    return false;
  }

  const std::vector<StartCode> codes = startCodes(bytes);
  std::vector<size_t> boundaries;
  for (const StartCode& code : codes) {
    if (isAud(bytes, code, codec)) boundaries.push_back(code.offset);
  }
  if (boundaries.empty() || boundaries.front() != 0) boundaries.insert(boundaries.begin(), 0);
  boundaries.push_back(bytes.size());

  const uint64_t interval = fps == 0 ? 33333 : 1000000ull / fps;
  for (size_t index = 0; index + 1 < boundaries.size(); ++index) {
    const size_t begin = boundaries[index];
    const size_t end = boundaries[index + 1];
    if (end <= begin) continue;
    const size_t length = end - begin;
    if (length > rc::wire::kMaxPayloadBytes) {
      reason = "one replay access unit exceeds the 16 MiB wire limit";
      units_.clear();
      return false;
    }
    EncodedUnit unit;
    unit.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(begin),
                        bytes.begin() + static_cast<std::ptrdiff_t>(end));
    const rc::annexb::AccessUnitReport report = rc::annexb::inspect(unit.payload, annexbCodec);
    unit.keyframe = report.hasKeyframeSlice;
    unit.frameNumber = units_.size();
    unit.ptsMicros = unit.frameNumber * interval;
    units_.push_back(std::move(unit));
  }
  if (units_.empty()) {
    reason = "no access units found";
    return false;
  }
  durationMicros_ = static_cast<uint64_t>(units_.size()) * interval;
  return true;
}

EncodedUnit ReplayMedia::next() {
  if (units_.empty()) return {};
  EncodedUnit result = units_[cursor_];
  result.ptsMicros += cycle_ * durationMicros_;
  ++cursor_;
  if (cursor_ == units_.size()) {
    cursor_ = 0;
    ++cycle_;
  }
  return result;
}

}  // namespace rcfakephone
