#include "rcfakephone/scenario_engine.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include "rc/annexb.h"
#include "rc/control.h"
#include "rc/wire.h"
#include "rcfakephone/synthetic_media.h"
#include "rcfakephone/replay_media.h"
#include "rcnet/tcp_client.h"
#include "rcwin/hr.h"

namespace rcfakephone {
namespace {

constexpr uint64_t kPendingVideoBudget = 20ull * 1024ull * 1024ull;

uint64_t monotonicMicros() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

std::string hrText(HRESULT hr) {
  const std::wstring wide = rcwin::hrMessage(hr);
  if (wide.empty()) return "unknown error";
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (size <= 0) return "HRESULT " + std::to_string(static_cast<unsigned long>(hr));
  std::string out(static_cast<size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size,
                        nullptr, nullptr);
  return out;
}

HRESULT sendControl(rcnet::TcpClient& client, const rc::control::Message& message,
                    rc::wire::Channel channel = rc::wire::Channel::Control) {
  const std::vector<uint8_t> payload = message.encode();
  return client.send(static_cast<uint8_t>(channel), 0, monotonicMicros(), payload.data(),
                     payload.size());
}

bool decodeControl(const rc::wire::Frame& frame, rc::control::Message& message,
                   std::string& reason) {
  rc::cbor::Error cborError = rc::cbor::Error::None;
  const rc::control::Error error =
      rc::control::Message::decode(frame.payload, message, cborError);
  if (error == rc::control::Error::None) return true;
  reason = std::string(rc::control::errorText(error)) + " / " + rc::cbor::errorText(cborError);
  return false;
}

bool needsControls(const std::string& scenario) {
  return scenario == "controls" || scenario == "control-conformance";
}

bool needsStats(const std::string& scenario) {
  return scenario == "adaptive" || scenario == "backpressure";
}

bool needsKeyframeRequest(const std::string& scenario) {
  return scenario == "media-recovery";
}

std::string endpointText(const Endpoint& endpoint) {
  return endpoint.host + ":" + std::to_string(endpoint.port);
}

}  // namespace

Pcg32::Pcg32(uint64_t seed)
    : state_(0), increment_((seed << 1u) | 1u) {
  next();
  state_ += seed ^ 0x853c49e6748fea9bull;
  next();
}

uint32_t Pcg32::next() {
  const uint64_t old = state_;
  state_ = old * 6364136223846793005ull + increment_;
  const uint32_t shifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
  const uint32_t rotation = static_cast<uint32_t>(old >> 59u);
  return (shifted >> rotation) | (shifted << ((0u - rotation) & 31u));
}

uint32_t Pcg32::bounded(uint32_t bound) {
  if (bound == 0) return 0;
  const uint32_t threshold = static_cast<uint32_t>(0u - bound) % bound;
  for (;;) {
    const uint32_t value = next();
    if (value >= threshold) return value % bound;
  }
}

bool ScenarioEngine::knownScenario(const std::string& name) {
  static const std::set<std::string> names = {
      "smoke",          "production-lock", "controls",     "adaptive",
      "reconnect",      "wire-conformance", "control-conformance",
      "media-recovery", "backpressure",    "chaos",        "soak",
  };
  return names.find(name) != names.end();
}

std::string ScenarioEngine::scenarioList() {
  return "smoke, production-lock, controls, adaptive, reconnect, wire-conformance, "
         "control-conformance, media-recovery, backpressure, chaos, soak";
}

ScenarioResult ScenarioEngine::run(const ScenarioOptions& options) {
  reporter_.info("scenario.start",
                 options.scenario + " -> " + endpointText(options.endpoint) +
                     " seed=" + std::to_string(options.seed));
  ScenarioResult result;
  if (!knownScenario(options.scenario)) {
    reporter_.failure("scenario.unknown", "known scenarios: " + scenarioList());
    result.protocolFailures = 1;
    return result;
  }
  if (!validDeviceId(options.profile.deviceId)) {
    reporter_.failure("profile.device_id",
                      "device id must be exactly 16 lowercase hexadecimal characters");
    result.protocolFailures = 1;
    return result;
  }

  if (options.scenario == "wire-conformance") {
    result = runWireConformance(options);
  } else if (options.scenario == "chaos") {
    result = runChaos(options);
  } else if (options.scenario == "reconnect") {
    result = runReconnect(options);
  } else {
    result = runSession(options);
  }
  reporter_.event(result.passed ? EventLevel::Pass : EventLevel::Failure,
                  "scenario.finish",
                  options.scenario + " frames=" + std::to_string(result.videoFramesSent) +
                      " bytes=" + std::to_string(result.videoBytesSent));
  return result;
}

ScenarioResult ScenarioEngine::runSession(const ScenarioOptions& options) {
  ScenarioResult result;
  PhoneModel model(options.profile);
  rcnet::TcpClient client;
  const HRESULT connectHr = client.connect(options.endpoint.host, options.endpoint.port);
  if (FAILED(connectHr)) {
    reporter_.failure("transport.connect",
                      endpointText(options.endpoint) + ": " + hrText(connectHr));
    result.protocolFailures = 1;
    result.connectionFailed = true;
    return result;
  }
  model.connected();
  reporter_.pass("transport.connect", "TCP_NODELAY client connected to " + client.peer());

  const HRESULT helloHr =
      sendControl(client, rc::control::hello(model.profile().deviceName,
                                              model.profile().deviceId,
                                              model.profile().model,
                                              model.profile().capabilities.codecs));
  if (FAILED(helloHr)) {
    reporter_.failure("handshake.hello", hrText(helloHr));
    result.protocolFailures = 1;
    return result;
  }
  model.helloSent();
  reporter_.pass("handshake.hello", "sent protocol v1 iOS-compatible identity");

  bool sawServerInfo = false;
  bool sawReady = false;
  bool startupSent = false;
  bool forceKeyframe = false;
  bool sawStats = false;
  bool sawKeyframeRequest = false;
  std::set<std::string> controlFamilies;
  const auto started = std::chrono::steady_clock::now();
  const auto deadline = started + std::chrono::milliseconds(options.durationMillis);
  auto nextFrameAt = started;
  SyntheticMedia media(rc::control::conservativeDefault(), monotonicMicros());
  ReplayMedia replay;
  const uint64_t replayPtsBase = monotonicMicros();
  if (!options.replayFile.empty()) {
    std::string reason;
    if (!replay.load(options.replayFile, options.replayCodec, 30, reason)) {
      reporter_.failure("media.replay", reason);
      ++result.protocolFailures;
      client.close();
      return result;
    }
    reporter_.pass("media.replay",
                   "loaded " + std::to_string(replay.size()) + " access units");
  }

  while (std::chrono::steady_clock::now() < deadline) {
    rc::wire::Frame incoming;
    const HRESULT receiveHr = client.receive(incoming, 10);
    if (SUCCEEDED(receiveHr)) {
      if (incoming.channel == static_cast<uint8_t>(rc::wire::Channel::Audio)) {
        reporter_.info("wire.audio", "ignored reserved v1 audio channel");
        continue;
      }
      if (incoming.channel != static_cast<uint8_t>(rc::wire::Channel::Control) &&
          incoming.channel != static_cast<uint8_t>(rc::wire::Channel::Stats)) {
        reporter_.warning("wire.channel",
                          "ignored unknown server channel " +
                              std::to_string(incoming.channel));
        continue;
      }

      rc::control::Message message;
      std::string decodeReason;
      if (!decodeControl(incoming, message, decodeReason)) {
        reporter_.warning("control.malformed", decodeReason);
        continue;
      }
      reporter_.info("control.receive", message.type);
      const ControlEffects effects = model.apply(message);
      if (!effects.accepted) {
        reporter_.failure("control.reject", message.type + ": " + effects.reason);
        ++result.protocolFailures;
        break;
      }
      if (message.type == "server_info") {
        sawServerInfo = true;
        bool paired = false;
        message.boolean("paired", paired);
        reporter_.pass("handshake.server_info",
                       paired ? "server reports paired" : "server reports paired=false");
      }
      if (effects.becameReady) {
        if (!options.allowInsecureSession) {
          reporter_.failure(
              "security.unauthenticated_ready",
              "server attempted to start a session without the explicit test-only bypass");
          ++result.protocolFailures;
          result.securityBlocked = true;
          break;
        }
        sawReady = true;
        if (!options.replayFile.empty() && model.streamConfig().codec != options.replayCodec) {
          reporter_.failure("media.replay_codec",
                            "server ready codec does not match the replay elementary stream");
          ++result.protocolFailures;
          break;
        }
        media.reconfigure(model.streamConfig());
        reporter_.pass("handshake.ready",
                       std::string(rc::control::codecName(model.streamConfig().codec)) + " " +
                           std::to_string(model.streamConfig().width) + "x" +
                           std::to_string(model.streamConfig().height) + "@" +
                           std::to_string(model.streamConfig().fps));
        for (const rc::control::Message& startup : model.startupMessages()) {
          const HRESULT sendHr = sendControl(client, startup);
          if (FAILED(sendHr)) {
            reporter_.failure("startup.send", startup.type + ": " + hrText(sendHr));
            ++result.protocolFailures;
            break;
          }
        }
        if (result.protocolFailures != 0) break;
        startupSent = true;
        reporter_.pass("stream.startup",
                       "sent caps, camera state, stream_start and telemetry");
      }
      if (effects.formatChanged) {
        media.reconfigure(model.streamConfig());
        forceKeyframe = true;
        controlFamilies.insert("set_format");
        reporter_.pass("control.set_format", "reconfigured stream and scheduled keyframe");
      }
      if (effects.cameraChanged || effects.controlsChanged) {
        const HRESULT echoHr = sendControl(client, rc::control::cameraState(model.profile().camera));
        if (FAILED(echoHr)) {
          reporter_.failure("control.echo", hrText(echoHr));
          ++result.protocolFailures;
          break;
        }
        controlFamilies.insert(effects.cameraChanged ? "set_camera" : "set_control");
      }
      if (effects.previewChanged) controlFamilies.insert("set_preview");
      if (effects.keyframeRequested) {
        forceKeyframe = true;
        sawKeyframeRequest = message.type == "request_keyframe" || sawKeyframeRequest;
        controlFamilies.insert("request_keyframe");
      }
      if (effects.statsReceived) {
        sawStats = true;
        reporter_.pass("adaptive.stats",
                       "target bitrate=" + std::to_string(model.targetBitrate()));
      }
    } else if (receiveHr != HRESULT_FROM_WIN32(ERROR_TIMEOUT)) {
      // A production-lock run is expected to remain connected but idle. Every other
      // run treats an early close as a backend failure.
      if (options.scenario != "production-lock") {
        reporter_.failure("transport.receive", hrText(receiveHr));
        ++result.protocolFailures;
        result.connectionFailed = true;
      }
      break;
    }

    if (startupSent && std::chrono::steady_clock::now() >= nextFrameAt) {
      EncodedUnit unit = replay.empty() ? media.next(forceKeyframe) : replay.next();
      if (!replay.empty()) unit.ptsMicros += replayPtsBase;
      forceKeyframe = false;
      const rc::annexb::Codec codec = model.streamConfig().codec == rc::control::Codec::Hevc
                                          ? rc::annexb::Codec::Hevc
                                          : rc::annexb::Codec::H264;
      const rc::annexb::AccessUnitReport inspection = rc::annexb::inspect(unit.payload, codec);
      if (!inspection.isAnnexB || (unit.keyframe && !inspection.decodableFromHere)) {
        reporter_.failure("media.fixture", "generated keyframe is not independently framed");
        ++result.protocolFailures;
        break;
      }

      // backpressure sends large access units as quickly as the blocking socket permits,
      // but never allocates above the iOS transport's 20 MiB pending-video budget.
      if (options.scenario == "backpressure") {
        const size_t target = 256 * 1024;
        if (unit.payload.size() < target) unit.payload.resize(target, 0x55);
      }
      if (unit.payload.size() + rc::wire::kHeaderBytes > kPendingVideoBudget) {
        reporter_.warning("media.backpressure", "dropped access unit above 20 MiB budget");
      } else {
        const HRESULT sendHr = client.send(
            static_cast<uint8_t>(rc::wire::Channel::Video),
            unit.keyframe ? rc::wire::flags::kKeyframe : 0, unit.ptsMicros,
            unit.payload.data(), unit.payload.size());
        if (FAILED(sendHr)) {
          reporter_.failure("media.send", hrText(sendHr));
          ++result.protocolFailures;
          break;
        }
        ++result.videoFramesSent;
        result.videoBytesSent += unit.payload.size();
      }

      if (options.scenario == "backpressure" || !options.realtime) {
        nextFrameAt = std::chrono::steady_clock::now();
      } else {
        const uint32_t fps = std::max<uint32_t>(1, model.streamConfig().fps);
        nextFrameAt += std::chrono::microseconds(1000000 / fps);
      }
    }
  }

  client.close();
  model.disconnected();

  if (!sawServerInfo) {
    reporter_.failure("handshake.server_info", "server never answered hello");
    ++result.protocolFailures;
  }
  if (options.scenario == "production-lock") {
    if (sawReady) {
      reporter_.failure("security.production_lock", "production endpoint sent ready");
      ++result.protocolFailures;
    } else if (sawServerInfo && result.protocolFailures == 0) {
      reporter_.pass("security.production_lock",
                     "paired=false boundary held and no video was sent");
    }
  } else {
    if (!sawReady) {
      reporter_.failure("handshake.ready",
                        "no ready message; use production-lock for a shipping endpoint");
      ++result.protocolFailures;
      if (model.state() == SessionState::AwaitingTrust) result.securityBlocked = true;
    }
    if (sawReady && result.videoFramesSent == 0) {
      reporter_.failure("media.video", "stream started but no access unit was sent");
      ++result.protocolFailures;
    }
  }

  if (needsControls(options.scenario)) {
    for (const char* family : {"set_preview", "set_camera", "set_control", "set_format"}) {
      if (controlFamilies.find(family) == controlFamilies.end()) {
        reporter_.failure("control.coverage", std::string("server never sent ") + family);
        ++result.protocolFailures;
      }
    }
  }
  if (needsStats(options.scenario) && !sawStats) {
    reporter_.failure("adaptive.coverage", "server never sent target_bitrate stats");
    ++result.protocolFailures;
  }
  if (needsKeyframeRequest(options.scenario) && !sawKeyframeRequest) {
    reporter_.failure("media.recovery", "server never requested a recovery keyframe");
    ++result.protocolFailures;
  }

  result.passed = result.protocolFailures == 0;
  return result;
}

ScenarioResult ScenarioEngine::runWireConformance(const ScenarioOptions& options) {
  ScenarioResult result;
  const auto expectClose = [&](const std::vector<uint8_t>& bytes,
                               const std::string& caseName) -> bool {
    rcnet::TcpClient client;
    const HRESULT connectHr = client.connect(options.endpoint.host, options.endpoint.port);
    if (FAILED(connectHr)) {
      reporter_.failure("wire." + caseName, "connect failed: " + hrText(connectHr));
      return false;
    }
    if (FAILED(client.sendRaw(bytes))) {
      reporter_.failure("wire." + caseName, "raw send failed");
      return false;
    }
    rc::wire::Frame ignored;
    const HRESULT receiveHr = client.receive(ignored, 1500);
    client.close();
    if (receiveHr == HRESULT_FROM_WIN32(ERROR_TIMEOUT) || SUCCEEDED(receiveHr)) {
      reporter_.failure("wire." + caseName,
                        "backend did not close after a fatal framing violation");
      return false;
    }
    reporter_.pass("wire." + caseName, "backend closed the invalid stream");
    return true;
  };

  const uint32_t oversized = rc::wire::kMaxPayloadBytes + 1;
  std::vector<uint8_t> oversizedHeader = {
      static_cast<uint8_t>(oversized >> 24), static_cast<uint8_t>(oversized >> 16),
      static_cast<uint8_t>(oversized >> 8),  static_cast<uint8_t>(oversized),
      1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  const std::vector<uint8_t> reservedFlag = {0, 0, 0, 0, 1, 0x80, 0, 0,
                                              0, 0, 0, 0, 0, 0, 0, 0};
  const std::vector<uint8_t> reservedHeader = {0, 0, 0, 0, 1, 0, 0, 1,
                                                0, 0, 0, 0, 0, 0, 0, 0};
  if (!expectClose(oversizedHeader, "oversized")) ++result.protocolFailures;
  if (!expectClose(reservedFlag, "reserved_flag")) ++result.protocolFailures;
  if (!expectClose(reservedHeader, "reserved_header")) ++result.protocolFailures;
  result.passed = result.protocolFailures == 0;
  return result;
}

ScenarioResult ScenarioEngine::runChaos(const ScenarioOptions& options) {
  ScenarioResult aggregate;
  Pcg32 random(options.seed);
  const uint64_t cases = std::max<uint64_t>(4, options.durationMillis / 250);
  for (uint64_t index = 0; index < cases; ++index) {
    rcnet::TcpClient client;
    const HRESULT connectHr = client.connect(options.endpoint.host, options.endpoint.port);
    if (FAILED(connectHr)) {
      reporter_.failure("chaos.connect", hrText(connectHr));
      ++aggregate.protocolFailures;
      break;
    }
    const uint32_t fault = index < 4 ? static_cast<uint32_t>(index) : random.bounded(4);
    bool fatal = false;
    if (fault == 0) {
      const std::vector<uint8_t> malformed = {0xff, 0xff, 0xff};
      client.send(static_cast<uint8_t>(rc::wire::Channel::Control), 0, monotonicMicros(),
                  malformed.data(), malformed.size());
      reporter_.pass("chaos.malformed_control", "sent isolated invalid CBOR");
    } else if (fault == 1) {
      const std::vector<uint8_t> audio = {1, 2, 3, 4};
      client.send(static_cast<uint8_t>(rc::wire::Channel::Audio), 0, monotonicMicros(),
                  audio.data(), audio.size());
      reporter_.pass("chaos.audio", "sent reserved audio channel");
    } else if (fault == 2) {
      const std::vector<uint8_t> unknown = {0x42};
      client.send(99, 0, monotonicMicros(), unknown.data(), unknown.size());
      reporter_.pass("chaos.unknown_channel", "sent additive unknown channel");
    } else {
      std::vector<uint8_t> header = {0, 0, 0, 0, 1, 0x80, 0, 0,
                                     0, 0, 0, 0, 0, 0, 0, 0};
      client.sendRaw(header);
      reporter_.pass("chaos.fatal_header", "sent reserved flag bit");
      fatal = true;
    }

    rc::wire::Frame response;
    if (fatal) {
      const HRESULT receiveHr = client.receive(response, 1500);
      if (SUCCEEDED(receiveHr) || receiveHr == HRESULT_FROM_WIN32(ERROR_TIMEOUT)) {
        reporter_.failure("chaos.fatal_header",
                          "backend did not close after the fatal frame violation");
        ++aggregate.protocolFailures;
      } else {
        reporter_.pass("chaos.fatal_header", "backend closed the invalid stream");
      }
    } else {
      const rc::control::Message hello =
          rc::control::hello(options.profile.deviceName, options.profile.deviceId,
                             options.profile.model, options.profile.capabilities.codecs);
      if (FAILED(sendControl(client, hello))) {
        reporter_.failure("chaos.recovery", "could not send hello after the additive fault");
        ++aggregate.protocolFailures;
      } else {
        bool recovered = false;
        for (int attempt = 0; attempt < 3; ++attempt) {
          if (FAILED(client.receive(response, 500))) break;
          if (response.channel != static_cast<uint8_t>(rc::wire::Channel::Control)) continue;
          rc::control::Message message;
          std::string reason;
          if (decodeControl(response, message, reason) && message.type == "server_info") {
            recovered = true;
            break;
          }
        }
        if (recovered) {
          reporter_.pass("chaos.recovery", "backend remained framed and answered hello");
        } else {
          reporter_.failure("chaos.recovery",
                            "backend failed to recover after a nonfatal additive fault");
          ++aggregate.protocolFailures;
        }
      }
    }
    client.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  aggregate.passed = aggregate.protocolFailures == 0;
  return aggregate;
}

ScenarioResult ScenarioEngine::runReconnect(const ScenarioOptions& options) {
  ScenarioResult aggregate;
  ScenarioOptions attempt = options;
  attempt.scenario = "smoke";
  attempt.durationMillis = std::max<uint64_t>(300, options.durationMillis / 3);
  const ScenarioResult first = runSession(attempt);
  aggregate.videoFramesSent += first.videoFramesSent;
  aggregate.videoBytesSent += first.videoBytesSent;
  aggregate.protocolFailures += first.protocolFailures;
  if (!first.passed) return aggregate;

  reporter_.info("reconnect.backoff", "waiting 1 second before the first retry");
  std::this_thread::sleep_for(std::chrono::seconds(1));
  const ScenarioResult second = runSession(attempt);
  aggregate.videoFramesSent += second.videoFramesSent;
  aggregate.videoBytesSent += second.videoBytesSent;
  aggregate.protocolFailures += second.protocolFailures;
  if (second.passed) {
    reporter_.pass("reconnect.identity",
                   "reconnected with the same stable 16-hex device identity");
  }
  aggregate.passed = first.passed && second.passed;
  return aggregate;
}

}  // namespace rcfakephone
