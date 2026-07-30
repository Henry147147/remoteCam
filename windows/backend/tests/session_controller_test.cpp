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

class RecordingConsumer final : public rcbackend::IEncodedConsumer {
 public:
  explicit RecordingConsumer(bool accepts) : accepts_(accepts) {}
  bool consume(const rcbackend::EncodedAccessUnit& unit) override {
    ++calls_;
    if (unit.keyframe) ++keyframes_;
    return accepts_;
  }
  int calls() const { return calls_.load(); }
  int keyframes() const { return keyframes_.load(); }

 private:
  bool accepts_ = true;
  std::atomic<int> calls_{0};
  std::atomic<int> keyframes_{0};
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

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  testTrustedWalkingSkeleton();
  testProductionBoundary();
  testBoundedQueueRecovery();
  testHelloTimeout();
  testStreamProgressTimeout();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
