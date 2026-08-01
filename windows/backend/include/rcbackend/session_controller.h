// Auth-gated Windows backend session shared by the shipping app and E2E hosts.

#ifndef RCBACKEND_SESSION_CONTROLLER_H
#define RCBACKEND_SESSION_CONTROLLER_H

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "rc/control.h"
#include "rcnet/tcp_listener.h"
#include "rcsecurity/security.h"

namespace rcbackend {

enum class State { Disconnected, AwaitingHello, AwaitingTrust, Ready, Streaming, Failed };

struct Metrics {
  State state = State::Disconnected;
  uint64_t connections = 0;
  uint64_t controlMessages = 0;
  uint64_t videoFrames = 0;
  uint64_t videoBytes = 0;
  uint64_t malformedMessages = 0;
  uint64_t droppedFrames = 0;
  size_t queueDepth = 0;
  size_t queueBytes = 0;
  bool trusted = false;
  bool waitingForKeyframe = true;
  uint64_t streamGeneration = 0;
  uint64_t pendingFormatGeneration = 0;
};

class ITrustPolicy {
 public:
  virtual ~ITrustPolicy() = default;
  // A policy that grants trust without ISessionSecurity is granting it without proof.
  // Only the user-selected pairing opt-out in windows/app does that; this library
  // deliberately ships no such implementation.
  virtual bool trusted(const rc::control::Hello& hello) = 0;
  // This PC's standing preference, reported in server_info even to a phone that did not
  // opt in, so the phone can say which end is refusing instead of stalling silently.
  virtual bool allowsUnauthenticated() const { return false; }
};

class RejectingTrustPolicy final : public ITrustPolicy {
 public:
  bool trusted(const rc::control::Hello&) override { return false; }
};

class IClock {
 public:
  using TimePoint = std::chrono::steady_clock::time_point;
  virtual ~IClock() = default;
  virtual TimePoint now() const = 0;
};

class SystemClock final : public IClock {
 public:
  TimePoint now() const override { return std::chrono::steady_clock::now(); }
};

class IBackendObserver {
 public:
  virtual ~IBackendObserver() = default;
  virtual void onBackendEvent(const std::string& kind, const std::string& detail) = 0;
};

struct EncodedAccessUnit {
  std::vector<uint8_t> bytes;
  rc::control::Codec codec = rc::control::Codec::H264;
  bool keyframe = false;
  uint64_t ptsMicros = 0;
  // Zero is the initial READY configuration. Live SET_FORMAT transitions use a
  // positive generation acknowledged by the phone before any unit is accepted.
  uint64_t generation = 0;
};

class IEncodedConsumer {
 public:
  virtual ~IEncodedConsumer() = default;
  // False means temporarily busy: the bounded queue retains this and later units.
  virtual bool consume(const EncodedAccessUnit& unit) = 0;
  // Called synchronously before the first access unit of a connection or acknowledged
  // format generation can be delivered. Existing consumers may ignore the callback,
  // while decoder-backed consumers use it to flush and rebuild codec state.
  virtual void reset(const rc::control::StreamConfig&, uint64_t) {}
};

struct SessionConfig {
  std::string serverName = "Windows PC";
  std::string serviceId = "0000000000000000";
  rc::control::StreamConfig initialStream = rc::control::conservativeDefault();
  std::chrono::milliseconds helloTimeout{5000};
  std::chrono::milliseconds trustTimeout{10000};
  std::chrono::milliseconds progressTimeout{10000};
  std::chrono::milliseconds formatAckTimeout{5000};
  std::chrono::milliseconds videoTimeout{5000};
  std::chrono::milliseconds idleTimeout{15000};
  size_t maxQueuedAccessUnits = 8;
  size_t maxQueuedBytes = 20u * 1024u * 1024u;
  bool encryptMedia = false;
};

class SessionController final : public rcnet::SessionHandler {
 public:
  SessionController(SessionConfig config, ITrustPolicy& trustPolicy,
                    IEncodedConsumer* consumer = nullptr,
                    IBackendObserver* observer = nullptr, IClock* clock = nullptr);
  SessionController(SessionConfig config, rcsecurity::ISessionSecurity& sessionSecurity,
                    IEncodedConsumer* consumer = nullptr,
                    IBackendObserver* observer = nullptr, IClock* clock = nullptr);
  ~SessionController() override;

  SessionController(const SessionController&) = delete;
  SessionController& operator=(const SessionController&) = delete;

  void onConnected(rcnet::Connection& connection) override;
  void onFrame(rcnet::Connection& connection, const rc::wire::Frame& frame) override;
  void onDisconnected(rcnet::Connection& connection, HRESULT reason) override;

  Metrics metrics() const;
  std::optional<rc::control::Hello> peerHello() const;
  std::optional<rc::control::Caps> peerCaps() const;

  HRESULT sendPreview(bool enabled);
  HRESULT sendCamera(const std::string& lens, const std::optional<std::string>& position);
  HRESULT sendControls(const rc::control::CameraControls& controls);
  HRESULT sendFormat(const rc::control::StreamConfig& config);
  HRESULT sendStats(const rc::control::Stats& stats);
  HRESULT requestKeyframe();
  void setConsumer(IEncodedConsumer* consumer);

 private:
  HRESULT sendControl(const rc::control::Message& message,
                      rc::wire::Channel channel = rc::wire::Channel::Control);
  HRESULT sendControl(rcnet::Connection& connection, const rc::control::Message& message,
                      rc::wire::Channel channel = rc::wire::Channel::Control);
  void handleControl(rcnet::Connection& connection, const rc::wire::Frame& frame);
  void handleHello(rcnet::Connection& connection, const rc::control::Message& message);
  void handleAuthResponse(rcnet::Connection& connection,
                          const rc::control::Message& message);
  void handleFormatAck(rcnet::Connection& connection,
                       const rc::control::Message& message);
  void handleFormatReject(rcnet::Connection& connection,
                          const rc::control::Message& message);
  void handleVideo(rcnet::Connection& connection, const rc::wire::Frame& frame);
  void protocolFailure(rcnet::Connection& connection, const std::string& detail);
  void malformedControl(const std::string& detail);
  void resetConsumer(const rc::control::StreamConfig& config, uint64_t generation);
  void drainQueue();
  void resetQueue(bool countDrops);
  void resetQueueLocked(bool countDrops);
  void notify(const std::string& kind, const std::string& detail) const;
  void startWatchdog();
  void stopWatchdog();

  SessionConfig config_;
  ITrustPolicy* trustPolicy_ = nullptr;
  rcsecurity::ISessionSecurity* sessionSecurity_ = nullptr;
  std::atomic<std::shared_ptr<rcsecurity::ISessionProtector>> protector_;
  SystemClock ownedClock_;
  IClock* clock_ = nullptr;
  IBackendObserver* observer_ = nullptr;

  mutable std::mutex mutex_;
  // Consumer callbacks may block and therefore never run under mutex_, but consume()
  // and reset() must still be mutually exclusive across UI/network threads.
  std::mutex consumerMutex_;
  rcnet::Connection* connection_ = nullptr;
  IEncodedConsumer* consumer_ = nullptr;
  Metrics metrics_;
  std::optional<rc::control::Hello> hello_;
  std::optional<rc::control::Caps> caps_;
  rc::control::StreamConfig streamConfig_ = rc::control::conservativeDefault();
  struct QueuedAccessUnit {
    uint64_t token = 0;
    EncodedAccessUnit unit;
  };
  std::deque<QueuedAccessUnit> queue_;
  std::optional<rc::control::StreamConfig> pendingStreamConfig_;
  uint64_t nextFormatGeneration_ = 1;
  uint64_t nextQueueToken_ = 1;
  bool draining_ = false;
  bool resettingConsumer_ = false;
  IClock::TimePoint connectedAt_{};
  IClock::TimePoint trustAt_{};
  IClock::TimePoint readyAt_{};
  IClock::TimePoint pendingFormatAt_{};
  IClock::TimePoint streamingAt_{};
  IClock::TimePoint lastVideoAt_{};
  IClock::TimePoint lastActivity_{};
  bool receivedVideo_ = false;
  std::jthread watchdog_;
};

}  // namespace rcbackend

#endif  // RCBACKEND_SESSION_CONTROLLER_H
