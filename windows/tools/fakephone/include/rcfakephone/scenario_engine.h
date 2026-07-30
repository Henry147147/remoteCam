#ifndef RCFAKEPHONE_SCENARIO_ENGINE_H
#define RCFAKEPHONE_SCENARIO_ENGINE_H

#include <cstdint>
#include <filesystem>
#include <string>

#include "rcfakephone/phone_model.h"
#include "rcfakephone/reporter.h"

namespace rcfakephone {

struct Endpoint {
  std::string host = "127.0.0.1";
  uint16_t port = 7890;
};

struct ScenarioOptions {
  Endpoint endpoint;
  std::string scenario = "smoke";
  uint64_t seed = 0x72656d6f74656361ull;
  uint64_t durationMillis = 2000;
  PhoneProfile profile = standardProfile();
  bool allowInsecureSession = false;
  bool realtime = true;
  std::filesystem::path replayFile;
  rc::control::Codec replayCodec = rc::control::Codec::H264;
};

struct ScenarioResult {
  bool passed = false;
  int protocolFailures = 0;
  uint64_t videoFramesSent = 0;
  uint64_t videoBytesSent = 0;
  bool connectionFailed = false;
  bool securityBlocked = false;
};

// Tiny, deterministic generator used for repeatable chaos decisions. The algorithm and
// stream selector are pinned here so a report's seed always reproduces the same run.
class Pcg32 {
 public:
  explicit Pcg32(uint64_t seed);
  uint32_t next();
  uint32_t bounded(uint32_t bound);

 private:
  uint64_t state_ = 0;
  uint64_t increment_ = 0;
};

class ScenarioEngine {
 public:
  explicit ScenarioEngine(Reporter& reporter) : reporter_(reporter) {}

  ScenarioResult run(const ScenarioOptions& options);

  static bool knownScenario(const std::string& name);
  static std::string scenarioList();

 private:
  ScenarioResult runSession(const ScenarioOptions& options);
  ScenarioResult runWireConformance(const ScenarioOptions& options);
  ScenarioResult runChaos(const ScenarioOptions& options);
  ScenarioResult runReconnect(const ScenarioOptions& options);

  Reporter& reporter_;
};

}  // namespace rcfakephone

#endif  // RCFAKEPHONE_SCENARIO_ENGINE_H
