#include "rcbackend/session_controller.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rcfakephone/reporter.h"
#include "rcfakephone/scenario_engine.h"
#include "rcnet/tcp_client.h"
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

class TestTrust final : public rcbackend::ITrustPolicy {
 public:
  bool trusted(const rc::control::Hello& hello) override {
    ++calls;
    lastId = hello.deviceId;
    return true;
  }
  int calls = 0;
  std::string lastId;
};

class MarkerProtector final : public rcsecurity::ISessionProtector {
 public:
  uint64_t expiresUnixSeconds() const override { return 4102444800ull; }

  rcsecurity::Error protectControl(uint8_t, uint8_t flags, uint64_t,
                                   const uint8_t* plaintext, size_t plaintextSize,
                                   rcsecurity::ProtectedPayload& out) override {
    out.flags = flags;
    out.payload.assign(1, 0xa5);
    if (plaintextSize != 0) {
      out.payload.insert(out.payload.end(), plaintext, plaintext + plaintextSize);
    }
    return rcsecurity::Error::None;
  }

  rcsecurity::Error unprotectControl(uint8_t, uint8_t, uint64_t,
                                     const uint8_t* envelope, size_t envelopeSize,
                                     rcsecurity::Bytes& plaintext) override {
    if (envelope == nullptr || envelopeSize < 1 || envelope[0] != 0xa5) {
      return rcsecurity::Error::AuthenticationFailed;
    }
    plaintext.assign(envelope + 1, envelope + envelopeSize);
    return rcsecurity::Error::None;
  }

  rcsecurity::Error protectMedia(uint8_t, uint8_t, uint64_t, const uint8_t*, size_t,
                                 rcsecurity::ProtectedPayload&) override {
    return rcsecurity::Error::InvalidArgument;
  }
  rcsecurity::Error unprotectMedia(uint8_t, uint8_t, uint64_t, const uint8_t*, size_t,
                                   rcsecurity::Bytes&) override {
    return rcsecurity::Error::InvalidArgument;
  }
};

class TestSessionSecurity final : public rcsecurity::ISessionSecurity {
 public:
  rcsecurity::Error beginAuthentication(
      std::string_view source, std::string_view deviceId, uint64_t nowUnixSeconds,
      rcsecurity::AuthenticationChallenge& challenge) override {
    ++begins;
    lastSource = source;
    lastDevice = deviceId;
    challenge.serverNonce.fill(0x33);
    challenge.expiresUnixSeconds = nowUnixSeconds + 10;
    return rcsecurity::Error::None;
  }

  rcsecurity::Error finishAuthentication(
      std::string_view source, std::string_view deviceId,
      const rcsecurity::Nonce32& clientNonce, const rcsecurity::Key32& clientProof,
      uint64_t, rcsecurity::AuthenticationResult& result) override {
    ++finishes;
    if (source != lastSource || deviceId != lastDevice || clientNonce[0] != 0x44 ||
        clientProof[0] != 0x55) {
      return rcsecurity::Error::AuthenticationFailed;
    }
    result.serverProof.fill(0x66);
    result.protector = std::make_shared<MarkerProtector>();
    return rcsecurity::Error::None;
  }

  int begins = 0;
  int finishes = 0;
  std::string lastSource;
  std::string lastDevice;
};

class RecordingConsumer final : public rcbackend::IEncodedConsumer {
 public:
  explicit RecordingConsumer(bool accepts) : accepts_(accepts) {}
  bool consume(const rcbackend::EncodedAccessUnit& unit) override {
    ++calls_;
    if (unit.keyframe) ++keyframes_;
    lastGeneration_.store(unit.generation);
    return accepts_;
  }
  void reset(const rc::control::StreamConfig& config, uint64_t generation) override {
    ++resets_;
    resetCodec_.store(static_cast<int>(config.codec));
    resetGeneration_.store(generation);
  }
  int calls() const { return calls_.load(); }
  int keyframes() const { return keyframes_.load(); }
  int resets() const { return resets_.load(); }
  uint64_t lastGeneration() const { return lastGeneration_.load(); }
  uint64_t resetGeneration() const { return resetGeneration_.load(); }
  rc::control::Codec resetCodec() const {
    return static_cast<rc::control::Codec>(resetCodec_.load());
  }

 private:
  bool accepts_ = true;
  std::atomic<int> calls_{0};
  std::atomic<int> keyframes_{0};
  std::atomic<int> resets_{0};
  std::atomic<uint64_t> lastGeneration_{0};
  std::atomic<uint64_t> resetGeneration_{0};
  std::atomic<int> resetCodec_{static_cast<int>(rc::control::Codec::H264)};
};

class RecordingObserver final : public rcbackend::IBackendObserver {
 public:
  void onBackendEvent(const std::string& kind, const std::string& detail) override {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(kind + ":" + detail);
  }
  bool contains(const std::string& prefix) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const std::string& event : events_) {
      if (event.starts_with(prefix)) return true;
    }
    return false;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::string> events_;
};

rcfakephone::ScenarioResult runPhone(rcnet::TcpListener& listener,
                                     const std::string& scenario = "smoke",
                                     uint64_t durationMillis = 250) {
  rcfakephone::Reporter reporter({{}, {}, true});
  rcfakephone::ScenarioEngine engine(reporter);
  rcfakephone::ScenarioOptions options;
  options.endpoint = {"127.0.0.1", listener.boundPort()};
  options.scenario = scenario;
  options.allowInsecureSession = true;
  options.realtime = false;
  options.durationMillis = durationMillis;
  return engine.run(options);
}

template <typename Predicate>
bool waitUntil(Predicate predicate, int millis = 2000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(millis);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

HRESULT sendControl(rcnet::TcpClient& phone, const rc::control::Message& message,
                    uint8_t flags = 0) {
  const std::vector<uint8_t> payload = message.encode();
  return phone.send(static_cast<uint8_t>(rc::wire::Channel::Control), flags, 0,
                    payload.data(), payload.size());
}

bool receiveControl(rcnet::TcpClient& phone, rc::control::Message& message,
                    int millis = 1000) {
  rc::wire::Frame frame;
  if (FAILED(phone.receive(frame, millis)) ||
      frame.channel != static_cast<uint8_t>(rc::wire::Channel::Control)) {
    return false;
  }
  rc::cbor::Error cborError = rc::cbor::Error::None;
  return rc::control::Message::decode(frame.payload, message, cborError) ==
         rc::control::Error::None;
}

bool receiveMarkerProtectedControl(rcnet::TcpClient& phone,
                                   rc::control::Message& message,
                                   int millis = 1000) {
  rc::wire::Frame frame;
  if (FAILED(phone.receive(frame, millis)) ||
      frame.channel != static_cast<uint8_t>(rc::wire::Channel::Control) ||
      frame.payload.empty() || frame.payload[0] != 0xa5) {
    return false;
  }
  rc::cbor::Error cborError = rc::cbor::Error::None;
  return rc::control::Message::decode(frame.payload.data() + 1,
                                      frame.payload.size() - 1, message, cborError) ==
         rc::control::Error::None;
}

HRESULT sendMarkerProtectedControl(rcnet::TcpClient& phone,
                                   const rc::control::Message& message) {
  std::vector<uint8_t> payload{0xa5};
  const std::vector<uint8_t> encoded = message.encode();
  payload.insert(payload.end(), encoded.begin(), encoded.end());
  return phone.send(static_cast<uint8_t>(rc::wire::Channel::Control), 0, 0,
                    payload.data(), payload.size());
}

bool startTrustedStream(rcnet::TcpClient& phone, rcnet::TcpListener& listener,
                        rcbackend::SessionController& controller) {
  if (FAILED(phone.connect("127.0.0.1", listener.boundPort()))) return false;
  if (FAILED(sendControl(phone, rc::control::hello("Test iPhone", "0123456789abcdef",
                                                   "iPhone", {"h264", "hevc"})))) {
    return false;
  }
  rc::control::Message message;
  if (!receiveControl(phone, message) || message.type != "server_info") return false;
  if (!receiveControl(phone, message) || message.type != "ready") return false;
  if (FAILED(sendControl(phone, rc::control::streamStart()))) return false;
  return waitUntil([&] { return controller.metrics().state == rcbackend::State::Streaming; });
}

std::vector<uint8_t> h264Keyframe(bool parameterSetsFirst = true) {
  const std::vector<uint8_t> sps = {0, 0, 0, 1, 0x67, 0x01};
  const std::vector<uint8_t> pps = {0, 0, 0, 1, 0x68, 0x01};
  const std::vector<uint8_t> idr = {0, 0, 1, 0x65, 0x01};
  std::vector<uint8_t> out;
  const auto append = [&](const std::vector<uint8_t>& nal) {
    out.insert(out.end(), nal.begin(), nal.end());
  };
  if (parameterSetsFirst) {
    append(sps);
    append(pps);
    append(idr);
  } else {
    append(idr);
    append(sps);
    append(pps);
  }
  return out;
}

std::vector<uint8_t> hevcKeyframe() {
  std::vector<uint8_t> out;
  const auto append = [&](uint8_t type) {
    out.insert(out.end(), {0, 0, 1, static_cast<uint8_t>(type << 1), 0x01});
  };
  append(32);
  append(33);
  append(34);
  append(19);
  return out;
}

void testTrustedWalkingSkeleton() {
  std::printf("Trusted walking skeleton\n");
  TestTrust trust;
  RecordingConsumer consumer(true);
  RecordingObserver observer;
  rcbackend::SessionConfig config;
  config.serverName = "E2E PC";
  config.serviceId = "0123456789abcdef";
  rcbackend::SessionController controller(config, trust, &consumer, &observer);
  rcnet::TcpListener listener;
  check(SUCCEEDED(listener.start(0, &controller, true)), "backend listener starts");
  const rcfakephone::ScenarioResult result = runPhone(listener);
  listener.stop();

  check(result.passed, "emulated phone completes the shared backend session");
  const rcbackend::Metrics metrics = controller.metrics();
  check(metrics.connections == 1 && metrics.controlMessages >= 7,
        "backend counts connection, startup and telemetry controls");
  check(metrics.videoFrames > 0 && metrics.videoBytes > 0,
        "backend accepts framed Annex-B access units");
  check(consumer.calls() > 0 && consumer.keyframes() > 0,
        "encoded consumer receives video including a recovery point");
  check(trust.calls == 1 && trust.lastId == "72636d756c61746f",
        "trust policy receives the stable phone identity exactly once");
  check(observer.contains("session.state:streaming"),
        "observer sees the deterministic streaming checkpoint");
}

void testProductionBoundary() {
  std::printf("Production boundary\n");
  rcbackend::RejectingTrustPolicy trust;
  rcbackend::SessionConfig config;
  config.serviceId = "0123456789abcdef";
  rcbackend::SessionController controller(config, trust);
  rcnet::TcpListener listener;
  check(SUCCEEDED(listener.start(0, &controller, true)), "production backend starts");

  rcfakephone::Reporter reporter({{}, {}, true});
  rcfakephone::ScenarioEngine engine(reporter);
  rcfakephone::ScenarioOptions options;
  options.endpoint = {"127.0.0.1", listener.boundPort()};
  options.scenario = "production-lock";
  options.durationMillis = 150;
  const rcfakephone::ScenarioResult result = engine.run(options);
  listener.stop();
  check(result.passed, "production controller reports paired=false and withholds ready");
  check(controller.metrics().videoFrames == 0, "no media crosses the rejected trust policy");
}

void testAuthenticatedSecurityBoundary() {
  std::printf("Authenticated security boundary\n");
  TestSessionSecurity security;
  rcbackend::SessionConfig config;
  config.serviceId = "0123456789abcdef";
  rcbackend::SessionController controller(config, security);
  rcnet::TcpListener listener;
  check(SUCCEEDED(listener.start(0, &controller, true)), "secure backend starts");
  rcnet::TcpClient phone;
  check(SUCCEEDED(phone.connect("127.0.0.1", listener.boundPort())),
        "secure phone connects");
  check(SUCCEEDED(sendControl(phone, rc::control::hello(
                                      "Secure iPhone", "fedcba9876543210", "iPhone",
                                      {"h264", "hevc"}))),
        "secure phone sends only its claimed identity");
  rc::control::Message message;
  check(receiveControl(phone, message) && message.type == "server_info",
        "record lookup reports paired before granting trust");
  check(receiveControl(phone, message) && message.type == "auth_challenge",
        "backend sends a nonce-bound authentication challenge");
  check(controller.metrics().state == rcbackend::State::AwaitingTrust &&
            !controller.metrics().trusted,
        "claimed identity alone does not grant trust");

  rc::control::AuthResponse response;
  response.clientNonce.fill(0x44);
  response.clientProof.fill(0x55);
  check(SUCCEEDED(sendControl(phone, rc::control::authResponse(response))),
        "phone sends the authentication proof in the cleartext handshake");
  check(receiveControl(phone, message) && message.type == "auth_confirm",
        "server proof is the final cleartext handshake message");
  check(receiveMarkerProtectedControl(phone, message) && message.type == "ready",
        "ready is sent only inside the installed session envelope");
  check(SUCCEEDED(sendMarkerProtectedControl(phone, rc::control::streamStart())),
        "phone sends post-authentication control through its envelope");
  check(waitUntil([&] {
          const rcbackend::Metrics metrics = controller.metrics();
          return metrics.state == rcbackend::State::Streaming && metrics.trusted;
        }),
        "verified proof transitions the backend to trusted streaming");
  check(security.begins == 1 && security.finishes == 1 &&
            security.lastDevice == "fedcba9876543210" &&
            security.lastSource == "127.0.0.1",
        "security receives canonical device and source identities exactly once");
  phone.close();
  listener.stop();
}

void testBoundedQueueRecovery() {
  std::printf("Bounded queue recovery\n");
  TestTrust trust;
  RecordingConsumer busy(false);
  RecordingObserver observer;
  rcbackend::SessionConfig config;
  config.serviceId = "0123456789abcdef";
  config.maxQueuedAccessUnits = 3;
  config.maxQueuedBytes = 1024 * 1024;
  rcbackend::SessionController controller(config, trust, &busy, &observer);
  rcnet::TcpListener listener;
  check(SUCCEEDED(listener.start(0, &controller, true)), "bounded backend starts");
  const rcfakephone::ScenarioResult result = runPhone(listener, "smoke", 300);
  listener.stop();
  check(result.passed, "sender remains healthy while the consumer is busy");
  const rcbackend::Metrics metrics = controller.metrics();
  check(metrics.queueDepth <= config.maxQueuedAccessUnits,
        "encoded queue never exceeds its access-unit bound");
  check(metrics.queueBytes <= config.maxQueuedBytes, "encoded queue never exceeds its byte bound");
  check(metrics.droppedFrames > 0, "overflow is counted rather than growing without bound");
  check(observer.contains("video.backpressure"), "overflow emits a recovery checkpoint");
}

void testHelloTimeout() {
  std::printf("Hello timeout\n");
  TestTrust trust;
  RecordingObserver observer;
  rcbackend::SessionConfig config;
  config.serviceId = "0123456789abcdef";
  config.helloTimeout = std::chrono::milliseconds(100);
  config.idleTimeout = std::chrono::seconds(5);
  rcbackend::SessionController controller(config, trust, nullptr, &observer);
  rcnet::TcpListener listener;
  check(SUCCEEDED(listener.start(0, &controller, true)), "timeout backend starts");
  rcnet::TcpClient silent;
  check(SUCCEEDED(silent.connect("127.0.0.1", listener.boundPort())), "silent peer connects");
  rc::wire::Frame ignored;
  const HRESULT receiveHr = silent.receive(ignored, 2000);
  check(FAILED(receiveHr) && receiveHr != HRESULT_FROM_WIN32(ERROR_TIMEOUT),
        "backend closes a peer that never sends hello");
  silent.close();
  listener.stop();
  check(observer.contains("session.timeout:hello timeout"), "timeout reason is observable");
}

void testStreamProgressTimeout() {
  std::printf("Stream progress timeout\n");
  TestTrust trust;
  RecordingObserver observer;
  rcbackend::SessionConfig config;
  config.serviceId = "0123456789abcdef";
  config.progressTimeout = std::chrono::milliseconds(100);
  config.idleTimeout = std::chrono::seconds(5);
  rcbackend::SessionController controller(config, trust, nullptr, &observer);
  rcnet::TcpListener listener;
  check(SUCCEEDED(listener.start(0, &controller, true)), "progress-timeout backend starts");
  rcnet::TcpClient stalled;
  check(SUCCEEDED(stalled.connect("127.0.0.1", listener.boundPort())), "stalled phone connects");
  const rc::control::Message hello =
      rc::control::hello("Stalled iPhone", "0123456789abcdef", "iPhone", {"h264"});
  const std::vector<uint8_t> payload = hello.encode();
  check(SUCCEEDED(stalled.send(static_cast<uint8_t>(rc::wire::Channel::Control), 0, 0,
                               payload.data(), payload.size())),
        "stalled phone sends hello");
  rc::wire::Frame frame;
  check(SUCCEEDED(stalled.receive(frame, 1000)), "stalled phone receives server_info");
  check(SUCCEEDED(stalled.receive(frame, 1000)), "stalled phone receives ready");
  const HRESULT receiveHr = stalled.receive(frame, 2000);
  check(FAILED(receiveHr) && receiveHr != HRESULT_FROM_WIN32(ERROR_TIMEOUT),
        "backend closes a trusted phone that never starts its stream");
  stalled.close();
  listener.stop();
  check(observer.contains("session.timeout:stream progress timeout"),
        "stream progress timeout reason is observable");
}

void testHandshakeAndFrameRules() {
  std::printf("Handshake, state and fragment rules\n");

  const auto expectClose = [](const rc::control::Message& message, uint8_t flags,
                              const std::string& what) {
    TestTrust trust;
    RecordingObserver observer;
    rcbackend::SessionConfig config;
    config.serviceId = "0123456789abcdef";
    rcbackend::SessionController controller(config, trust, nullptr, &observer);
    rcnet::TcpListener listener;
    check(SUCCEEDED(listener.start(0, &controller, true)), what + ": listener starts");
    rcnet::TcpClient phone;
    check(SUCCEEDED(phone.connect("127.0.0.1", listener.boundPort())),
          what + ": phone connects");
    check(SUCCEEDED(sendControl(phone, message, flags)), what + ": invalid frame is sent");
    rc::wire::Frame ignored;
    const HRESULT hr = phone.receive(ignored, 2000);
    check(FAILED(hr) && hr != HRESULT_FROM_WIN32(ERROR_TIMEOUT),
          what + ": backend closes the connection");
    phone.close();
    listener.stop();
    check(observer.contains("protocol.failure"), what + ": violation is observable");
  };

  rc::control::Message v0 =
      rc::control::hello("Old iPhone", "0123456789abcdef", "iPhone", {"h264"});
  v0.fields.insert_or_assign("v", rc::cbor::Value::unsignedInt(0));
  expectClose(v0, 0, "protocol v0");

  rc::control::Message uppercaseId =
      rc::control::hello("Aliased iPhone", "0123456789abcdef", "iPhone", {"h264"});
  uppercaseId.fields.insert_or_assign("device_id",
                                      rc::cbor::Value::text("0123456789ABCDEf"));
  expectClose(uppercaseId, 0, "non-canonical device id");

  expectClose(rc::control::streamStart(), 0, "stream_start before hello");
  expectClose(rc::control::hello("Fragmented iPhone", "0123456789abcdef", "iPhone",
                                 {"h264"}),
              rc::wire::flags::kEndOfFragment, "fragment flag");
}

void testTrustDeadline() {
  std::printf("Untrusted session deadline\n");
  rcbackend::RejectingTrustPolicy trust;
  RecordingObserver observer;
  rcbackend::SessionConfig config;
  config.serviceId = "0123456789abcdef";
  config.trustTimeout = std::chrono::milliseconds(100);
  config.idleTimeout = std::chrono::seconds(5);
  rcbackend::SessionController controller(config, trust, nullptr, &observer);
  rcnet::TcpListener listener;
  check(SUCCEEDED(listener.start(0, &controller, true)), "untrusted backend starts");
  rcnet::TcpClient phone;
  check(SUCCEEDED(phone.connect("127.0.0.1", listener.boundPort())), "phone connects");
  check(SUCCEEDED(sendControl(phone, rc::control::hello(
                                         "Unpaired iPhone", "0123456789abcdef",
                                         "iPhone", {"h264"}))),
        "unpaired phone sends hello");
  rc::control::Message serverInfo;
  check(receiveControl(phone, serverInfo) && serverInfo.type == "server_info",
        "unpaired phone receives only server_info");
  const std::vector<uint8_t> ignoredAudio = {1, 2, 3};
  check(SUCCEEDED(phone.send(static_cast<uint8_t>(rc::wire::Channel::Audio), 0, 0,
                             ignoredAudio.data(), ignoredAudio.size())),
        "ignored audio cannot count as trust progress");
  rc::wire::Frame ignored;
  const HRESULT receiveHr = phone.receive(ignored, 2000);
  check(FAILED(receiveHr) && receiveHr != HRESULT_FROM_WIN32(ERROR_TIMEOUT),
        "untrusted connection closes on its absolute deadline");
  phone.close();
  listener.stop();
  check(observer.contains("session.timeout:trust timeout"),
        "trust deadline reason is observable");
}

void testKeyframeRecoveryUsesBitstream() {
  std::printf("Keyframe recovery uses the bitstream\n");
  TestTrust trust;
  RecordingConsumer consumer(true);
  RecordingObserver observer;
  rcbackend::SessionConfig config;
  config.serviceId = "0123456789abcdef";
  rcbackend::SessionController controller(config, trust, &consumer, &observer);
  rcnet::TcpListener listener;
  check(SUCCEEDED(listener.start(0, &controller, true)), "keyframe backend starts");
  rcnet::TcpClient phone;
  check(startTrustedStream(phone, listener, controller), "trusted stream starts");
  check(waitUntil([&] { return consumer.resets() == 1; }),
        "decoder is reset before the initial stream");

  const std::vector<uint8_t> delta = {0, 0, 1, 0x61, 0x01};
  check(SUCCEEDED(phone.send(static_cast<uint8_t>(rc::wire::Channel::Video),
                             rc::wire::flags::kKeyframe, 1, delta.data(), delta.size())),
        "a falsely flagged delta frame is sent");
  check(waitUntil([&] { return controller.metrics().droppedFrames >= 1; }),
        "flag/bitstream mismatch is dropped");
  check(consumer.calls() == 0, "mislabeled delta never reaches the decoder");
  rc::control::Message request;
  check(receiveControl(phone, request) && request.type == "request_keyframe",
        "mislabeled delta triggers recovery");

  const std::vector<uint8_t> late = h264Keyframe(false);
  check(SUCCEEDED(phone.send(static_cast<uint8_t>(rc::wire::Channel::Video),
                             rc::wire::flags::kKeyframe, 2, late.data(), late.size())),
        "a keyframe with late parameter sets is sent");
  check(waitUntil([&] { return controller.metrics().droppedFrames >= 2; }),
        "late parameter sets do not clear recovery");
  check(receiveControl(phone, request) && request.type == "request_keyframe",
        "late parameter sets trigger another recovery request");

  const std::vector<uint8_t> good = h264Keyframe();
  check(SUCCEEDED(phone.send(static_cast<uint8_t>(rc::wire::Channel::Video),
                             rc::wire::flags::kKeyframe, 3, good.data(), good.size())),
        "a self-contained keyframe is sent");
  check(waitUntil([&] { return consumer.calls() == 1; }),
        "only the self-contained keyframe reaches the decoder");
  check(!controller.metrics().waitingForKeyframe, "valid recovery clears the wait state");
  phone.close();
  listener.stop();
}

void testGenerationAcknowledgedReconfiguration() {
  std::printf("Generation-acknowledged reconfiguration\n");
  TestTrust trust;
  RecordingConsumer consumer(true);
  RecordingObserver observer;
  rcbackend::SessionConfig config;
  config.serviceId = "0123456789abcdef";
  rcbackend::SessionController controller(config, trust, &consumer, &observer);
  rcnet::TcpListener listener;
  check(SUCCEEDED(listener.start(0, &controller, true)), "reconfiguration backend starts");
  rcnet::TcpClient phone;
  check(startTrustedStream(phone, listener, controller), "initial stream starts");

  const std::vector<uint8_t> initial = h264Keyframe();
  check(SUCCEEDED(phone.send(static_cast<uint8_t>(rc::wire::Channel::Video),
                             rc::wire::flags::kKeyframe, 10,
                             initial.data(), initial.size())),
        "initial generation keyframe is sent");
  check(waitUntil([&] { return consumer.calls() == 1; }),
        "initial generation reaches the consumer");
  check(consumer.lastGeneration() == 0 && consumer.resetGeneration() == 0,
        "initial stream is generation zero");

  rc::control::StreamConfig changed = config.initialStream;
  changed.codec = rc::control::Codec::Hevc;
  check(SUCCEEDED(controller.sendFormat(changed)), "backend sends a live format change");
  rc::control::Message setFormat;
  check(receiveControl(phone, setFormat) && setFormat.type == "set_format",
        "phone receives set_format");
  uint64_t generation = 0;
  check(setFormat.unsignedInt("generation", generation) && generation == 1,
        "set_format carries the next generation");
  check(controller.metrics().pendingFormatGeneration == generation,
        "backend records the pending generation");
  check(consumer.resets() == 1, "decoder is not rebuilt before acknowledgement");

  check(SUCCEEDED(phone.send(static_cast<uint8_t>(rc::wire::Channel::Video),
                             rc::wire::flags::kKeyframe, 11,
                             initial.data(), initial.size())),
        "old generation frame races the acknowledgement");
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  check(consumer.calls() == 1, "media is gated while a format generation is pending");

  check(SUCCEEDED(sendControl(phone, rc::control::formatAck(generation))),
        "phone acknowledges the applied generation");
  rc::control::Message request;
  check(receiveControl(phone, request) && request.type == "request_keyframe",
        "backend requests a post-reset keyframe");
  check(waitUntil([&] { return consumer.resets() == 2; }),
        "decoder resets only after the matching acknowledgement");
  check(consumer.resetGeneration() == generation &&
            consumer.resetCodec() == rc::control::Codec::Hevc,
        "decoder reset carries the acknowledged codec and generation");

  const std::vector<uint8_t> hevc = hevcKeyframe();
  check(SUCCEEDED(phone.send(static_cast<uint8_t>(rc::wire::Channel::Video),
                             rc::wire::flags::kKeyframe, 12, hevc.data(), hevc.size())),
        "new generation keyframe is sent");
  check(waitUntil([&] { return consumer.calls() == 2; }),
        "new generation reaches the consumer after reset");
  const rcbackend::Metrics metrics = controller.metrics();
  check(consumer.lastGeneration() == generation && metrics.streamGeneration == generation &&
            metrics.pendingFormatGeneration == 0,
        "only the acknowledged generation becomes active");
  phone.close();
  listener.stop();
}

void testFormatAcknowledgementDeadline() {
  std::printf("Format acknowledgement deadline\n");
  TestTrust trust;
  RecordingObserver observer;
  rcbackend::SessionConfig config;
  config.serviceId = "0123456789abcdef";
  config.formatAckTimeout = std::chrono::milliseconds(100);
  config.videoTimeout = std::chrono::seconds(5);
  config.idleTimeout = std::chrono::seconds(5);
  rcbackend::SessionController controller(config, trust, nullptr, &observer);
  rcnet::TcpListener listener;
  check(SUCCEEDED(listener.start(0, &controller, true)), "ack-timeout backend starts");
  rcnet::TcpClient phone;
  check(startTrustedStream(phone, listener, controller), "stream starts");
  rc::control::StreamConfig changed = config.initialStream;
  changed.bitrate += 1000000;
  check(SUCCEEDED(controller.sendFormat(changed)), "format change is sent");
  rc::control::Message setFormat;
  check(receiveControl(phone, setFormat) && setFormat.type == "set_format",
        "phone receives the generation it will not acknowledge");
  rc::wire::Frame ignored;
  const HRESULT receiveHr = phone.receive(ignored, 2000);
  check(FAILED(receiveHr) && receiveHr != HRESULT_FROM_WIN32(ERROR_TIMEOUT),
        "missing format acknowledgement closes the session");
  phone.close();
  listener.stop();
  check(observer.contains("session.timeout:format acknowledgement timeout"),
        "format acknowledgement timeout is observable");
}

void testFormatRejectionRollback() {
  std::printf("Format rejection rollback\n");
  TestTrust trust;
  RecordingConsumer consumer(true);
  RecordingObserver observer;
  rcbackend::SessionConfig config;
  config.serviceId = "0123456789abcdef";
  rcbackend::SessionController controller(config, trust, &consumer, &observer);
  rcnet::TcpListener listener;
  check(SUCCEEDED(listener.start(0, &controller, true)), "rollback backend starts");
  rcnet::TcpClient phone;
  check(startTrustedStream(phone, listener, controller), "rollback stream starts");

  const std::vector<uint8_t> initial = h264Keyframe();
  check(SUCCEEDED(phone.send(static_cast<uint8_t>(rc::wire::Channel::Video),
                             rc::wire::flags::kKeyframe, 10,
                             initial.data(), initial.size())),
        "initial rollback generation keyframe is sent");
  check(waitUntil([&] { return consumer.calls() == 1; }),
        "initial rollback generation reaches the consumer");

  rc::control::StreamConfig changed = config.initialStream;
  changed.codec = rc::control::Codec::Hevc;
  check(SUCCEEDED(controller.sendFormat(changed)), "rollback format change is sent");
  rc::control::Message setFormat;
  check(receiveControl(phone, setFormat) && setFormat.type == "set_format",
        "rollback phone receives set_format");
  uint64_t generation = 0;
  check(setFormat.unsignedInt("generation", generation) && generation == 1,
        "rollback request carries generation one");

  const rc::control::FormatReject rejection{
      generation, "encoder_rebuild_failed", "simulated encoder rejection"};
  check(SUCCEEDED(sendControl(phone, rc::control::formatReject(rejection))),
        "phone rejects the pending format generation");
  rc::control::Message request;
  check(receiveControl(phone, request) && request.type == "request_keyframe",
        "rollback requests a committed-format recovery keyframe");
  check(waitUntil([&] {
          const rcbackend::Metrics metrics = controller.metrics();
          return metrics.pendingFormatGeneration == 0 && metrics.waitingForKeyframe;
        }),
        "rejection clears only the pending generation");
  check(controller.metrics().streamGeneration == 0 && consumer.resets() == 1,
        "rejection keeps generation zero and does not rebuild the decoder");
  check(observer.contains("stream.format_rejected:generation 1: encoder_rebuild_failed"),
        "format rejection is observable");

  check(SUCCEEDED(phone.send(static_cast<uint8_t>(rc::wire::Channel::Video),
                             rc::wire::flags::kKeyframe, 11,
                             initial.data(), initial.size())),
        "committed-format recovery keyframe is sent");
  check(waitUntil([&] { return consumer.calls() == 2; }),
        "committed H.264 stream resumes after rejection");
  check(consumer.lastGeneration() == 0 &&
            controller.metrics().state == rcbackend::State::Streaming,
        "rollback remains streaming on the committed generation");
  phone.close();
  listener.stop();
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  testTrustedWalkingSkeleton();
  testProductionBoundary();
  testAuthenticatedSecurityBoundary();
  testBoundedQueueRecovery();
  testHelloTimeout();
  testStreamProgressTimeout();
  testHandshakeAndFrameRules();
  testTrustDeadline();
  testKeyframeRecoveryUsesBitstream();
  testGenerationAcknowledgedReconfiguration();
  testFormatAcknowledgementDeadline();
  testFormatRejectionRollback();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
