#include "rcfakephone/phone_model.h"
#include "rcfakephone/replay_media.h"
#include "rcfakephone/scenario_engine.h"
#include "rcfakephone/synthetic_media.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rc/annexb.h"
#include "rc/control.h"
#include "rcnet/tcp_listener.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool condition, const std::string& what) {
  ++g_checks;
  if (!condition) {
    ++g_failures;
    std::printf("  FAIL: %s\n", what.c_str());
  }
}

HRESULT sendControl(rcnet::Connection& connection, const rc::control::Message& message,
                    rc::wire::Channel channel = rc::wire::Channel::Control) {
  const std::vector<uint8_t> payload = message.encode();
  return connection.send(static_cast<uint8_t>(channel), 0, 0, payload.data(), payload.size());
}

class Harness final : public rcnet::SessionHandler {
 public:
  explicit Harness(bool productionLock = false) : productionLock_(productionLock) {}

  void onConnected(rcnet::Connection&) override {}

  void onFrame(rcnet::Connection& connection, const rc::wire::Frame& frame) override {
    if (frame.channel == static_cast<uint8_t>(rc::wire::Channel::Video)) {
      const rc::annexb::AccessUnitReport report =
          rc::annexb::inspect(frame.payload, rc::annexb::Codec::H264);
      if (report.isAnnexB) ++videoFrames_;
      if (frame.isKeyframe() && report.decodableFromHere) ++independentKeyframes_;
      return;
    }
    if (frame.channel != static_cast<uint8_t>(rc::wire::Channel::Control) &&
        frame.channel != static_cast<uint8_t>(rc::wire::Channel::Stats)) {
      return;
    }
    rc::control::Message message;
    rc::cbor::Error cborError = rc::cbor::Error::None;
    if (rc::control::Message::decode(frame.payload, message, cborError) !=
        rc::control::Error::None) {
      return;
    }
    if (message.type == "hello") {
      ++hellos_;
      sendControl(connection,
                  rc::control::serverInfo("E2E Backend", "0123456789abcdef",
                                          !productionLock_, {"h264", "hevc"}));
      if (!productionLock_) {
        sendControl(connection, rc::control::ready(rc::control::conservativeDefault()));
      }
    } else if (message.type == "caps") {
      ++caps_;
      sendExerciseCommands(connection);
    } else if (message.type == "stream_start") {
      ++streamStarts_;
    } else if (message.type == "camera_state") {
      ++cameraStates_;
    } else if (message.type == "orientation" || message.type == "thermal" ||
               message.type == "battery") {
      ++telemetry_;
    }
  }

  void onDisconnected(rcnet::Connection&, HRESULT) override {}

  int hellos() const { return hellos_.load(); }
  int caps() const { return caps_.load(); }
  int streamStarts() const { return streamStarts_.load(); }
  int cameraStates() const { return cameraStates_.load(); }
  int telemetry() const { return telemetry_.load(); }
  int videoFrames() const { return videoFrames_.load(); }
  int independentKeyframes() const { return independentKeyframes_.load(); }

 private:
  void sendExerciseCommands(rcnet::Connection& connection) {
    if (commandsSent_.exchange(true)) return;
    sendControl(connection, rc::control::setPreview(false));
    sendControl(connection, rc::control::setCamera("wide", std::string("back")));
    rc::control::CameraControls controls;
    controls.zoom = 2.0;
    controls.focusMode = "manual";
    controls.focus = 0.25;
    controls.torch = true;
    sendControl(connection, rc::control::setControl(controls));
    rc::control::StreamConfig format = rc::control::conservativeDefault();
    format.bitrate = 5000000;
    sendControl(connection, rc::control::setFormat(format));
    rc::control::Stats stats;
    stats.targetBitrate = 3000000;
    sendControl(connection, rc::control::stats(stats), rc::wire::Channel::Stats);
    sendControl(connection, rc::control::requestKeyframe());
  }

  bool productionLock_ = false;
  std::atomic<bool> commandsSent_{false};
  std::atomic<int> hellos_{0};
  std::atomic<int> caps_{0};
  std::atomic<int> streamStarts_{0};
  std::atomic<int> cameraStates_{0};
  std::atomic<int> telemetry_{0};
  std::atomic<int> videoFrames_{0};
  std::atomic<int> independentKeyframes_{0};
};

void testPhoneModel() {
  std::printf("Phone model\n");
  check(rcfakephone::validDeviceId("0123456789abcdef"), "stable lowercase identity accepted");
  check(!rcfakephone::validDeviceId("0123456789ABCDEF"), "uppercase identity rejected");
  check(!rcfakephone::validDeviceId("short"), "short identity rejected");

  rcfakephone::PhoneModel model;
  check(model.profile().capabilities.cameras.size() == 4, "standard profile has lens coverage");
  check(model.startupMessages().size() == 6, "startup burst has every iOS message family");

  const rcfakephone::ControlEffects ready =
      model.apply(rc::control::ready(rc::control::conservativeDefault()));
  check(ready.accepted && ready.becameReady && model.state() == rcfakephone::SessionState::Ready,
        "valid ready advances the phone state");

  rcfakephone::PhoneProfile constrained = rcfakephone::constrainedProfile();
  check(constrained.capabilities.codecs.size() == 1 &&
            constrained.capabilities.codecs[0] == "h264" &&
            constrained.thermal.state == "serious",
        "constrained profile exposes codec, thermal and battery pressure");
}

void testSyntheticMedia() {
  std::printf("Synthetic media\n");
  rc::control::StreamConfig h264 = rc::control::conservativeDefault();
  rcfakephone::SyntheticMedia h264Media(h264, 1000);
  const rcfakephone::EncodedUnit first = h264Media.next();
  const rc::annexb::AccessUnitReport h264Report =
      rc::annexb::inspect(first.payload, rc::annexb::Codec::H264);
  check(first.keyframe && h264Report.decodableFromHere,
        "first H.264 access unit has SPS/PPS and IDR");
  const rcfakephone::EncodedUnit second = h264Media.next();
  check(!second.keyframe && second.ptsMicros > first.ptsMicros,
        "delta unit advances the monotonic PTS");

  rc::control::StreamConfig hevc = h264;
  hevc.codec = rc::control::Codec::Hevc;
  rcfakephone::SyntheticMedia hevcMedia(hevc);
  const rcfakephone::EncodedUnit hevcFirst = hevcMedia.next();
  check(rc::annexb::inspect(hevcFirst.payload, rc::annexb::Codec::Hevc).decodableFromHere,
        "first HEVC access unit has VPS/SPS/PPS and IDR");
}

void testPcgDeterminism() {
  std::printf("PCG32 determinism\n");
  rcfakephone::Pcg32 first(42);
  rcfakephone::Pcg32 second(42);
  rcfakephone::Pcg32 different(43);
  bool same = true;
  bool diverged = false;
  for (int index = 0; index < 32; ++index) {
    const uint32_t a = first.next();
    const uint32_t b = second.next();
    const uint32_t c = different.next();
    same = same && a == b;
    diverged = diverged || a != c;
  }
  check(same, "the same seed reproduces exactly");
  check(diverged, "a different seed changes the chaos stream");
}

void testReplayMedia() {
  std::printf("Replay media\n");
  rc::control::StreamConfig config = rc::control::conservativeDefault();
  rcfakephone::SyntheticMedia media(config);
  const rcfakephone::EncodedUnit keyframe = media.next();
  const rcfakephone::EncodedUnit delta = media.next();
  const std::vector<uint8_t> aud = {0, 0, 0, 1, 0x09, 0xf0};
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() /
      ("rc-fakephone-replay-" + std::to_string(::GetCurrentProcessId()) + ".h264");
  {
    std::ofstream output(path, std::ios::out | std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(aud.data()),
                 static_cast<std::streamsize>(aud.size()));
    output.write(reinterpret_cast<const char*>(keyframe.payload.data()),
                 static_cast<std::streamsize>(keyframe.payload.size()));
    output.write(reinterpret_cast<const char*>(aud.data()),
                 static_cast<std::streamsize>(aud.size()));
    output.write(reinterpret_cast<const char*>(delta.payload.data()),
                 static_cast<std::streamsize>(delta.payload.size()));
  }
  rcfakephone::ReplayMedia replay;
  std::string reason;
  check(replay.load(path, rc::control::Codec::H264, 30, reason),
        "AUD-delimited Annex-B fixture loads");
  check(replay.size() == 2, "AUD delimiters produce one wire access unit each");
  const rcfakephone::EncodedUnit first = replay.next();
  const rcfakephone::EncodedUnit second = replay.next();
  const rcfakephone::EncodedUnit wrapped = replay.next();
  check(first.keyframe && !second.keyframe, "replay derives keyframe flags from NALs");
  check(wrapped.frameNumber == first.frameNumber && wrapped.ptsMicros > second.ptsMicros,
        "replay loops deterministically while keeping PTS monotonic");
  std::error_code removeError;
  std::filesystem::remove(path, removeError);
  check(!removeError, "temporary replay fixture is removed");
}

void testIntegrationScenario(const std::string& scenario) {
  std::printf("Integration scenario: %s\n", scenario.c_str());
  Harness handler;
  rcnet::TcpListener listener;
  check(SUCCEEDED(listener.start(0, &handler, true)), "loopback backend starts");

  rcfakephone::Reporter reporter({{}, {}, true});
  rcfakephone::ScenarioEngine engine(reporter);
  rcfakephone::ScenarioOptions options;
  options.endpoint = {"127.0.0.1", listener.boundPort()};
  options.scenario = scenario;
  options.allowInsecureSession = true;
  options.realtime = false;
  options.durationMillis = 250;
  const rcfakephone::ScenarioResult result = engine.run(options);
  listener.stop();

  check(result.passed, scenario + " passes against the real loopback receiver");
  check(handler.hellos() == 1 && handler.caps() == 1 && handler.streamStarts() == 1,
        "handshake and startup messages cross the socket");
  check(handler.cameraStates() >= 1 && handler.telemetry() == 3,
        "camera state and all telemetry families cross the socket");
  check(handler.videoFrames() > 0 && handler.independentKeyframes() > 0,
        "backend receives Annex-B video and an independently decodable keyframe");
}

void testProductionLock() {
  std::printf("Production security lock\n");
  Harness handler(true);
  rcnet::TcpListener listener;
  check(SUCCEEDED(listener.start(0, &handler, true)), "production-like backend starts");

  rcfakephone::Reporter reporter({{}, {}, true});
  rcfakephone::ScenarioEngine engine(reporter);
  rcfakephone::ScenarioOptions options;
  options.endpoint = {"127.0.0.1", listener.boundPort()};
  options.scenario = "production-lock";
  options.durationMillis = 100;
  const rcfakephone::ScenarioResult result = engine.run(options);
  listener.stop();
  check(result.passed, "paired=false with no ready is the expected production result");
  check(handler.videoFrames() == 0, "the emulator sends no video across the trust boundary");
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  testPhoneModel();
  testSyntheticMedia();
  testPcgDeterminism();
  testReplayMedia();
  testIntegrationScenario("smoke");
  testIntegrationScenario("controls");
  testIntegrationScenario("adaptive");
  testIntegrationScenario("media-recovery");
  testProductionLock();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
