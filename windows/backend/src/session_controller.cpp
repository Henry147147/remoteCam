#include "rcbackend/session_controller.h"

#include <utility>

#include "rc/annexb.h"
#include "rcwin/hr.h"

namespace rcbackend {
namespace {

const char* stateName(State state) {
  switch (state) {
    case State::Disconnected: return "disconnected";
    case State::AwaitingHello: return "awaiting_hello";
    case State::AwaitingTrust: return "awaiting_trust";
    case State::Ready: return "ready";
    case State::Streaming: return "streaming";
    case State::Failed: return "failed";
  }
  return "unknown";
}

}  // namespace

SessionController::SessionController(SessionConfig config, ITrustPolicy& trustPolicy,
                                     IEncodedConsumer* consumer,
                                     IBackendObserver* observer, IClock* clock)
    : config_(std::move(config)),
      trustPolicy_(trustPolicy),
      clock_(clock == nullptr ? &ownedClock_ : clock),
      observer_(observer),
      consumer_(consumer),
      streamConfig_(config_.initialStream) {}

SessionController::~SessionController() { stopWatchdog(); }

void SessionController::notify(const std::string& kind, const std::string& detail) const {
  if (observer_ != nullptr) observer_->onBackendEvent(kind, detail);
}

HRESULT SessionController::sendControl(rcnet::Connection& connection,
                                       const rc::control::Message& message,
                                       rc::wire::Channel channel) {
  const std::vector<uint8_t> payload = message.encode();
  return connection.send(static_cast<uint8_t>(channel), 0, 0, payload.data(), payload.size());
}

HRESULT SessionController::sendControl(const rc::control::Message& message,
                                       rc::wire::Channel channel) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (connection_ == nullptr || !metrics_.trusted) {
    return HRESULT_FROM_WIN32(ERROR_NOT_READY);
  }
  return sendControl(*connection_, message, channel);
}

void SessionController::onConnected(rcnet::Connection& connection) {
  stopWatchdog();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    connection_ = &connection;
    ++metrics_.connections;
    metrics_.state = State::AwaitingHello;
    metrics_.trusted = false;
    metrics_.waitingForKeyframe = true;
    hello_.reset();
    caps_.reset();
    queue_.clear();
    metrics_.queueDepth = 0;
    metrics_.queueBytes = 0;
    connectedAt_ = clock_->now();
    lastActivity_ = connectedAt_;
  }
  notify("session.state", stateName(State::AwaitingHello));
  startWatchdog();
}

void SessionController::onFrame(rcnet::Connection& connection,
                                const rc::wire::Frame& frame) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    lastActivity_ = clock_->now();
  }
  if (frame.channel == static_cast<uint8_t>(rc::wire::Channel::Audio)) return;
  if (frame.channel == static_cast<uint8_t>(rc::wire::Channel::Video)) {
    handleVideo(connection, frame);
    return;
  }
  if (frame.channel == static_cast<uint8_t>(rc::wire::Channel::Control) ||
      frame.channel == static_cast<uint8_t>(rc::wire::Channel::Stats)) {
    handleControl(connection, frame);
  }
}

void SessionController::handleControl(rcnet::Connection& connection,
                                      const rc::wire::Frame& frame) {
  rc::control::Message message;
  rc::cbor::Error cborError = rc::cbor::Error::None;
  const rc::control::Error error =
      rc::control::Message::decode(frame.payload, message, cborError);
  if (error != rc::control::Error::None) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++metrics_.malformedMessages;
    }
    notify("control.malformed", std::string(rc::control::errorText(error)) + ": " +
                                    rc::cbor::errorText(cborError));
    return;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.controlMessages;
  }

  if (message.type == "hello") {
    handleHello(connection, message);
    return;
  }

  bool trusted = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    trusted = metrics_.trusted;
  }
  if (!trusted) {
    notify("control.unauthenticated", message.type);
    return;
  }

  if (message.type == "caps") {
    rc::control::Caps parsed;
    if (rc::control::parseCaps(message, parsed)) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        caps_ = std::move(parsed);
      }
      notify("phone.caps", "received");
    }
  } else if (message.type == "stream_start") {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      metrics_.state = State::Streaming;
    }
    notify("session.state", stateName(State::Streaming));
  } else if (message.type == "camera_state" || message.type == "orientation" ||
             message.type == "thermal" || message.type == "battery" ||
             message.type == "error") {
    notify("phone." + message.type, "received");
  } else {
    notify("control.unknown", message.type);
  }
}

void SessionController::handleHello(rcnet::Connection& connection,
                                    const rc::control::Message& message) {
  rc::control::Hello parsed;
  if (!rc::control::parseHello(message, parsed)) {
    notify("handshake.failure", "hello is missing required fields");
    return;
  }
  if (parsed.version > rc::control::kProtocolVersion) {
    notify("handshake.failure", "peer protocol is newer than v1");
    connection.close();
    return;
  }
  const bool trusted = trustPolicy_.trusted(parsed);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    hello_ = parsed;
    metrics_.trusted = trusted;
    metrics_.state = trusted ? State::Ready : State::AwaitingTrust;
    if (trusted) readyAt_ = clock_->now();
  }

  const HRESULT infoHr = sendControl(
      connection,
      rc::control::serverInfo(config_.serverName, config_.serviceId, trusted, {"h264", "hevc"}));
  if (FAILED(infoHr)) {
    notify("handshake.failure", "could not send server_info");
    return;
  }
  if (trusted) {
    const HRESULT readyHr = sendControl(connection, rc::control::ready(streamConfig_));
    if (FAILED(readyHr)) {
      notify("handshake.failure", "could not send ready");
      return;
    }
  }
  notify("session.state", stateName(trusted ? State::Ready : State::AwaitingTrust));
}

void SessionController::handleVideo(rcnet::Connection& connection,
                                    const rc::wire::Frame& frame) {
  bool trusted = false;
  rc::control::Codec codec = rc::control::Codec::H264;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    trusted = metrics_.trusted;
    codec = streamConfig_.codec;
  }
  if (!trusted) {
    notify("video.unauthenticated", "ignored");
    return;
  }

  const rc::annexb::Codec annexbCodec = codec == rc::control::Codec::Hevc
                                            ? rc::annexb::Codec::Hevc
                                            : rc::annexb::Codec::H264;
  const rc::annexb::AccessUnitReport report = rc::annexb::inspect(frame.payload, annexbCodec);
  if (!report.isAnnexB || (frame.isKeyframe() && !report.decodableFromHere)) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++metrics_.droppedFrames;
      metrics_.waitingForKeyframe = true;
    }
    notify("video.invalid", report.isAnnexB ? "keyframe lacks parameter sets"
                                             : "payload is not Annex-B");
    sendControl(connection, rc::control::requestKeyframe());
    return;
  }

  bool overflow = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (metrics_.waitingForKeyframe && !frame.isKeyframe()) {
      ++metrics_.droppedFrames;
      return;
    }
    if (frame.isKeyframe()) metrics_.waitingForKeyframe = false;
    if (queue_.size() >= config_.maxQueuedAccessUnits ||
        metrics_.queueBytes + frame.payload.size() > config_.maxQueuedBytes) {
      metrics_.droppedFrames += queue_.size() + 1;
      queue_.clear();
      metrics_.queueDepth = 0;
      metrics_.queueBytes = 0;
      metrics_.waitingForKeyframe = true;
      overflow = true;
    } else {
      queue_.push_back({frame.payload, codec, frame.isKeyframe(), frame.ptsMicros});
      metrics_.queueDepth = queue_.size();
      metrics_.queueBytes += frame.payload.size();
      ++metrics_.videoFrames;
      metrics_.videoBytes += frame.payload.size();
    }
  }
  if (overflow) {
    notify("video.backpressure", "queue flushed; recovery keyframe requested");
    sendControl(connection, rc::control::requestKeyframe());
    return;
  }
  drainQueue();
}

void SessionController::drainQueue() {
  for (;;) {
    EncodedAccessUnit unit;
    IEncodedConsumer* consumer = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (queue_.empty() || consumer_ == nullptr) return;
      unit = queue_.front();
      consumer = consumer_;
    }
    if (!consumer->consume(unit)) return;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (queue_.empty()) return;
      metrics_.queueBytes -= queue_.front().bytes.size();
      queue_.pop_front();
      metrics_.queueDepth = queue_.size();
    }
  }
}

void SessionController::resetQueue(bool countDrops) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (countDrops) metrics_.droppedFrames += queue_.size();
  queue_.clear();
  metrics_.queueDepth = 0;
  metrics_.queueBytes = 0;
  metrics_.waitingForKeyframe = true;
}

void SessionController::onDisconnected(rcnet::Connection&, HRESULT reason) {
  stopWatchdog();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    connection_ = nullptr;
    metrics_.state = State::Disconnected;
    metrics_.trusted = false;
    hello_.reset();
    caps_.reset();
  }
  resetQueue(true);
  notify("session.state",
         reason == S_OK ? stateName(State::Disconnected) : "disconnected_with_error");
}

Metrics SessionController::metrics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return metrics_;
}

std::optional<rc::control::Hello> SessionController::peerHello() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return hello_;
}

std::optional<rc::control::Caps> SessionController::peerCaps() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return caps_;
}

HRESULT SessionController::sendPreview(bool enabled) {
  return sendControl(rc::control::setPreview(enabled));
}

HRESULT SessionController::sendCamera(const std::string& lens,
                                      const std::optional<std::string>& position) {
  return sendControl(rc::control::setCamera(lens, position));
}

HRESULT SessionController::sendControls(const rc::control::CameraControls& controls) {
  return sendControl(rc::control::setControl(controls));
}

HRESULT SessionController::sendFormat(const rc::control::StreamConfig& config) {
  if (!config.valid()) return E_INVALIDARG;
  const HRESULT hr = sendControl(rc::control::setFormat(config));
  if (SUCCEEDED(hr)) {
    std::lock_guard<std::mutex> lock(mutex_);
    streamConfig_ = config;
    metrics_.waitingForKeyframe = true;
  }
  return hr;
}

HRESULT SessionController::sendStats(const rc::control::Stats& stats) {
  return sendControl(rc::control::stats(stats), rc::wire::Channel::Stats);
}

HRESULT SessionController::requestKeyframe() {
  return sendControl(rc::control::requestKeyframe());
}

void SessionController::setConsumer(IEncodedConsumer* consumer) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    consumer_ = consumer;
  }
  drainQueue();
}

void SessionController::startWatchdog() {
  watchdog_ = std::jthread([this](std::stop_token stopToken) {
    while (!stopToken.stop_requested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      rcnet::Connection* expired = nullptr;
      std::string reason;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (connection_ == nullptr) return;
        const IClock::TimePoint now = clock_->now();
        if (metrics_.state == State::AwaitingHello && now - connectedAt_ > config_.helloTimeout) {
          expired = connection_;
          reason = "hello timeout";
        } else if (metrics_.state == State::Ready &&
                   now - readyAt_ > config_.progressTimeout) {
          expired = connection_;
          reason = "stream progress timeout";
        } else if (now - lastActivity_ > config_.idleTimeout) {
          expired = connection_;
          reason = "idle timeout";
        }
        if (expired != nullptr) metrics_.state = State::Failed;
      }
      if (expired != nullptr) {
        notify("session.timeout", reason);
        expired->close();
        return;
      }
    }
  });
}

void SessionController::stopWatchdog() {
  if (!watchdog_.joinable()) return;
  watchdog_.request_stop();
  if (watchdog_.get_id() != std::this_thread::get_id()) watchdog_.join();
}

}  // namespace rcbackend
