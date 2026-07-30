#ifndef RCFAKEPHONE_REPLAY_MEDIA_H
#define RCFAKEPHONE_REPLAY_MEDIA_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "rc/control.h"
#include "rcfakephone/synthetic_media.h"

namespace rcfakephone {

// Reads an Annex-B elementary stream whose access units are separated by AUD NALs.
// The fixture generator inserts those delimiters explicitly. A file without AUDs is
// accepted as one access unit so a captured keyframe remains useful for recovery tests.
class ReplayMedia {
 public:
  bool load(const std::filesystem::path& path, rc::control::Codec codec, uint32_t fps,
            std::string& reason);
  EncodedUnit next();
  bool empty() const { return units_.empty(); }
  size_t size() const { return units_.size(); }

 private:
  std::vector<EncodedUnit> units_;
  size_t cursor_ = 0;
  uint64_t cycle_ = 0;
  uint64_t durationMicros_ = 0;
};

}  // namespace rcfakephone

#endif  // RCFAKEPHONE_REPLAY_MEDIA_H
