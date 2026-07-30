#include "rcfakephone/synthetic_media.h"

#include <cstdio>
#include <string>

namespace rcfakephone {
namespace {

void appendNal(std::vector<uint8_t>& out, std::initializer_list<uint8_t> header,
               const std::vector<uint8_t>& body) {
  out.insert(out.end(), {0, 0, 0, 1});
  out.insert(out.end(), header);
  out.insert(out.end(), body.begin(), body.end());
}

}  // namespace

SyntheticMedia::SyntheticMedia(rc::control::StreamConfig config, uint64_t firstPtsMicros)
    : config_(config), firstPtsMicros_(firstPtsMicros) {}

void SyntheticMedia::reconfigure(rc::control::StreamConfig config) {
  config_ = config;
  frameNumber_ = 0;
}

std::vector<uint8_t> SyntheticMedia::marker() const {
  char buffer[128] = {};
  const int length = std::snprintf(buffer, sizeof(buffer),
                                   "RCFRAME=%08llu;SIZE=%ux%u;FPS=%u;",
                                   static_cast<unsigned long long>(frameNumber_), config_.width,
                                   config_.height, config_.fps);
  if (length <= 0) return {};
  return std::vector<uint8_t>(buffer, buffer + length);
}

std::vector<uint8_t> SyntheticMedia::makeH264(bool keyframe) const {
  std::vector<uint8_t> out;
  if (keyframe) {
    appendNal(out, {0x67}, {0x64, 0x00, 0x1f, 0xac, 0xd9, 0x40});  // SPS
    appendNal(out, {0x68}, {0xee, 0x3c, 0x80});                    // PPS
  }
  appendNal(out, {static_cast<uint8_t>(keyframe ? 0x65 : 0x41)}, marker());
  return out;
}

std::vector<uint8_t> SyntheticMedia::makeHevc(bool keyframe) const {
  std::vector<uint8_t> out;
  if (keyframe) {
    appendNal(out, {0x40, 0x01}, {0x0c, 0x01, 0xff});  // VPS, type 32
    appendNal(out, {0x42, 0x01}, {0x01, 0x60, 0x00});  // SPS, type 33
    appendNal(out, {0x44, 0x01}, {0xc0, 0x73});        // PPS, type 34
  }
  // IDR_W_RADL (19) or TRAIL_R (1), with nuh_temporal_id_plus1 = 1.
  appendNal(out, {static_cast<uint8_t>((keyframe ? 19 : 1) << 1), 0x01}, marker());
  return out;
}

EncodedUnit SyntheticMedia::next(bool forceKeyframe) {
  const uint64_t interval = config_.fps == 0 ? 33333 : 1000000ull / config_.fps;
  const uint64_t keyInterval = config_.fps == 0 ? 60 : static_cast<uint64_t>(config_.fps) * 2;
  const bool keyframe = forceKeyframe || frameNumber_ % keyInterval == 0;

  EncodedUnit unit;
  unit.frameNumber = frameNumber_;
  unit.ptsMicros = firstPtsMicros_ + frameNumber_ * interval;
  unit.keyframe = keyframe;
  unit.payload = config_.codec == rc::control::Codec::Hevc ? makeHevc(keyframe)
                                                           : makeH264(keyframe);
  ++frameNumber_;
  return unit;
}

}  // namespace rcfakephone
