#include "rcbackend/session_controller.h"

#include <array>
#include <chrono>
#include <cmath>
#include <limits>
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

bool isPcToPhoneMessage(const std::string& type) {
  constexpr std::array<const char*, 10> types = {
      "server_info", "ready",       "set_format", "set_camera",
      "set_control", "request_keyframe", "set_preview", "stats",
      "auth_challenge", "auth_confirm"};
  for (const char* candidate : types) {
    if (type == candidate) return true;
  }
  return false;
}

bool finiteCameraState(const rc::control::CameraState& state) {
  return std::isfinite(state.zoom) && std::isfinite(state.focus) &&
         std::isfinite(state.iso) && std::isfinite(state.exposureSeconds) &&
         std::isfinite(state.exposureBias) &&
         std::isfinite(state.whiteBalanceKelvin);
}

uint64_t unixNow() {
  const auto duration = std::chrono::system_clock::now().time_since_epoch();
  const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration).count();
  return seconds < 0 ? 0 : static_cast<uint64_t>(seconds);
}

std::string sourceAddress(const rcnet::Connection& connection) {
  const std::string& peer = connection.peer();
  const size_t separator = peer.rfind(':');
  return separator == std::string::npos ? peer : peer.substr(0, separator);
}

}  // namespace

SessionController::SessionController(SessionConfig config, ITrustPolicy& trustPolicy,
                                     IEncodedConsumer* consumer,
                                     IBackendObserver* observer, IClock* clock)
    : config_(std::move(config)),
      trustPolicy_(&trustPolicy),
      clock_(clock == nullptr ? &ownedClock_ : clock),
      observer_(observer),
      consumer_(consumer),
      streamConfig_(config_.initialStream) {}

SessionController::SessionController(SessionConfig config,
                                     rcsecurity::ISessionSecurity& sessionSecurity,
                                     IEncodedConsumer* consumer,
                                     IBackendObserver* observer, IClock* clock)
    : config_(std::move(config)),
      sessionSecurity_(&sessionSecurity),
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
  const std::shared_ptr<rcsecurity::ISessionProtector> protector = protector_.load();
  if (!protector) {
    return connection.send(static_cast<uint8_t>(channel), 0, 0,
                           payload.data(), payload.size());
  }
  rcsecurity::ProtectedPayload protectedPayload;
  const rcsecurity::Error error =
      channel == rc::wire::Channel::Stats && config_.encryptMedia
          ? protector->protectMedia(static_cast<uint8_t>(channel), 0, 0,
                                    payload.data(), payload.size(), protectedPayload)
          : protector->protectControl(static_cast<uint8_t>(channel), 0, 0,
                                      payload.data(), payload.size(), protectedPayload);
  if (error != rcsecurity::Error::None) {
    notify("security.protect_failed", rcsecurity::errorText(error));
    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
  }
  return connection.send(static_cast<uint8_t>(channel), protectedPayload.flags, 0,
                         protectedPayload.payload.data(), protectedPayload.payload.size());
}

HRESULT SessionController::sendControl(const rc::control::Message& message,
                                       rc::wire::Channel channel) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (connection_ == nullptr || !metrics_.trusted ||
      (metrics_.state != State::Ready && metrics_.state != State::Streaming)) {
    return HRESULT_FROM_WIN32(ERROR_NOT_READY);
  }
  return sendControl(*connection_, message, channel);
}

void SessionController::onConnected(rcnet::Connection& connection) {
  stopWatchdog();
  protector_.store(nullptr);
  bool configValid = false;
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
    pendingStreamConfig_.reset();
    nextFormatGeneration_ = 1;
    nextQueueToken_ = 1;
    draining_ = false;
    resettingConsumer_ = false;
    metrics_.queueDepth = 0;
    metrics_.queueBytes = 0;
    metrics_.streamGeneration = 0;
    metrics_.pendingFormatGeneration = 0;
    streamConfig_ = config_.initialStream;
    receivedVideo_ = false;
    connectedAt_ = clock_->now();
    lastActivity_ = connectedAt_;
    configValid = rc::control::validDeviceId(config_.serviceId) && streamConfig_.valid();
    if (!configValid) metrics_.state = State::Failed;
  }
  if (!configValid) {
    notify("session.configuration", "invalid service id or initial stream configuration");
    connection.close();
    return;
  }
  notify("session.state", stateName(State::AwaitingHello));
  startWatchdog();
}

void SessionController::onFrame(rcnet::Connection& connection,
                                const rc::wire::Frame& frame) {
  if ((frame.flags & rc::wire::flags::kEndOfFragment) != 0) {
    protocolFailure(connection, "fragmentation is forbidden in v1");
    return;
  }

  rc::wire::Frame authenticated = frame;
  const std::shared_ptr<rcsecurity::ISessionProtector> protector = protector_.load();
  if (protector) {
    rcsecurity::Bytes plaintext;
    rcsecurity::Error error = rcsecurity::Error::InvalidArgument;
    if (frame.channel == static_cast<uint8_t>(rc::wire::Channel::Control)) {
      error = protector->unprotectControl(frame.channel, frame.flags, frame.ptsMicros,
                                          frame.payload.data(), frame.payload.size(),
                                          plaintext);
    } else if (frame.channel == static_cast<uint8_t>(rc::wire::Channel::Video)) {
      if (config_.encryptMedia) {
        error = protector->unprotectMedia(frame.channel, frame.flags, frame.ptsMicros,
                                          frame.payload.data(), frame.payload.size(),
                                          plaintext);
      } else if ((frame.flags & rc::wire::flags::kEncrypted) == 0) {
        error = rcsecurity::Error::None;
        plaintext = frame.payload;
      }
    } else if (frame.channel == static_cast<uint8_t>(rc::wire::Channel::Stats)) {
      error = config_.encryptMedia
                  ? protector->unprotectMedia(frame.channel, frame.flags, frame.ptsMicros,
                                              frame.payload.data(), frame.payload.size(),
                                              plaintext)
                  : protector->unprotectControl(frame.channel, frame.flags, frame.ptsMicros,
                                                frame.payload.data(), frame.payload.size(),
                                                plaintext);
    } else if (frame.channel == static_cast<uint8_t>(rc::wire::Channel::Audio)) {
      return;  // reserved in v1 and never counts as progress
    }
    if (error != rcsecurity::Error::None) {
      protocolFailure(connection,
                      std::string("session envelope rejected: ") +
                          rcsecurity::errorText(error));
      return;
    }
    authenticated.payload = std::move(plaintext);
    authenticated.flags = static_cast<uint8_t>(
        authenticated.flags & ~rc::wire::flags::kEncrypted);
  } else if ((frame.flags & rc::wire::flags::kEncrypted) != 0) {
    protocolFailure(connection, "encrypted payload received before session authentication");
    return;
  }

  if (authenticated.channel != static_cast<uint8_t>(rc::wire::Channel::Video) &&
      authenticated.isKeyframe()) {
    protocolFailure(connection, "keyframe flag is valid only on the video channel");
    return;
  }
  if (authenticated.channel == static_cast<uint8_t>(rc::wire::Channel::Audio)) return;
  if (authenticated.channel == static_cast<uint8_t>(rc::wire::Channel::Video)) {
    handleVideo(connection, authenticated);
    return;
  }
  if (authenticated.channel == static_cast<uint8_t>(rc::wire::Channel::Control)) {
    handleControl(connection, authenticated);
    return;
  }
  if (authenticated.channel == static_cast<uint8_t>(rc::wire::Channel::Stats)) {
    protocolFailure(connection, "stats channel is PC-to-phone in v1");
    return;
  }
  protocolFailure(connection, "unknown channel has no authenticated v1 schema");
}

void SessionController::handleControl(rcnet::Connection& connection,
                                      const rc::wire::Frame& frame) {
  rc::control::Message message;
  rc::cbor::Error cborError = rc::cbor::Error::None;
  const rc::control::Error error =
      rc::control::Message::decode(frame.payload, message, cborError);
  if (error != rc::control::Error::None) {
    malformedControl(std::string(rc::control::errorText(error)) + ": " +
                     rc::cbor::errorText(cborError));
    return;
  }

  State state = State::Disconnected;
  bool trusted = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.controlMessages;
    state = metrics_.state;
    trusted = metrics_.trusted;
  }

  if (message.type == "hello") {
    if (state != State::AwaitingHello) {
      protocolFailure(connection, "hello is only valid as the first control message");
      return;
    }
    handleHello(connection, message);
    return;
  }

  if (state == State::AwaitingHello) {
    protocolFailure(connection, "first control message must be hello");
    return;
  }
  if (message.type == "auth_response") {
    if (state != State::AwaitingTrust || sessionSecurity_ == nullptr) {
      protocolFailure(connection, "auth_response is valid only during authentication");
      return;
    }
    handleAuthResponse(connection, message);
    return;
  }
  if (!trusted) {
    notify("control.unauthenticated", message.type);
    return;
  }
  if (state != State::Ready && state != State::Streaming) {
    protocolFailure(connection, "control message is invalid in the current session state");
    return;
  }
  if (isPcToPhoneMessage(message.type)) {
    protocolFailure(connection, "phone sent PC-to-phone message " + message.type);
    return;
  }
  if (message.type == "format_ack") {
    handleFormatAck(connection, message);
    return;
  }
  if (message.type == "format_reject") {
    handleFormatReject(connection, message);
    return;
  }

  if (message.type == "caps") {
    rc::control::Caps parsed;
    if (!rc::control::parseCaps(message, parsed)) {
      malformedControl("caps has an invalid v1 shape");
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      caps_ = std::move(parsed);
      lastActivity_ = clock_->now();
    }
    notify("phone.caps", "received");
  } else if (message.type == "stream_start") {
    if (!message.fields.empty()) {
      malformedControl("stream_start has unexpected v1 fields");
      return;
    }
    rc::control::StreamConfig config;
    uint64_t generation = 0;
    bool validTransition = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      validTransition = metrics_.state == State::Ready &&
                        !pendingStreamConfig_.has_value() && !resettingConsumer_;
      if (validTransition) {
        config = streamConfig_;
        generation = metrics_.streamGeneration;
        metrics_.state = State::Streaming;
        resettingConsumer_ = true;
        streamingAt_ = clock_->now();
        lastVideoAt_ = streamingAt_;
        lastActivity_ = streamingAt_;
        receivedVideo_ = false;
      }
    }
    if (!validTransition) {
      protocolFailure(connection, "stream_start is valid exactly once after ready");
      return;
    }
    resetConsumer(config, generation);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      resettingConsumer_ = false;
    }
    notify("stream.reset", "generation " + std::to_string(generation));
    notify("session.state", stateName(State::Streaming));
  } else if (message.type == "orientation") {
    rc::control::Orientation parsed;
    if (!rc::control::parseOrientation(message, parsed) || !std::isfinite(parsed.degrees)) {
      malformedControl("orientation has an invalid v1 shape");
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      lastActivity_ = clock_->now();
    }
    notify("phone.orientation", "received");
  } else if (message.type == "thermal") {
    rc::control::Thermal parsed;
    if (!rc::control::parseThermal(message, parsed) ||
        (parsed.state != "nominal" && parsed.state != "fair" &&
         parsed.state != "serious" && parsed.state != "critical")) {
      malformedControl("thermal has an invalid v1 shape");
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      lastActivity_ = clock_->now();
    }
    notify("phone.thermal", "received");
  } else if (message.type == "battery") {
    rc::control::Battery parsed;
    if (!rc::control::parseBattery(message, parsed) || !std::isfinite(parsed.level) ||
        parsed.level < 0.0 ||
        parsed.level > 1.0) {
      malformedControl("battery has an invalid v1 shape");
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      lastActivity_ = clock_->now();
    }
    notify("phone.battery", "received");
  } else if (message.type == "camera_state") {
    rc::control::CameraState parsed;
    if (!rc::control::parseCameraState(message, parsed) || !finiteCameraState(parsed)) {
      malformedControl("camera_state has an invalid v1 shape");
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      lastActivity_ = clock_->now();
    }
    notify("phone.camera_state", "received");
  } else if (message.type == "error") {
    rc::control::DeviceError parsed;
    if (!rc::control::parseError(message, parsed)) {
      malformedControl("error has an invalid v1 shape");
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      lastActivity_ = clock_->now();
    }
    notify("phone.error", parsed.code);
  } else {
    // Unknown additive types are ignored, but do not extend the idle deadline because
    // this version cannot know whether they represent real session progress.
    notify("control.unknown", message.type);
  }
}

void SessionController::protocolFailure(rcnet::Connection& connection,
                                        const std::string& detail) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_.state = State::Failed;
  }
  notify("protocol.failure", detail);
  connection.close();
}

void SessionController::malformedControl(const std::string& detail) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.malformedMessages;
  }
  notify("control.malformed", detail);
}

void SessionController::resetConsumer(const rc::control::StreamConfig& config,
                                      uint64_t generation) {
  std::lock_guard<std::mutex> callbackLock(consumerMutex_);
  IEncodedConsumer* consumer = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    consumer = consumer_;
  }
  if (consumer != nullptr) consumer->reset(config, generation);
}

void SessionController::handleFormatAck(rcnet::Connection& connection,
                                        const rc::control::Message& message) {
  uint64_t generation = 0;
  if (!rc::control::parseFormatAck(message, generation)) {
    malformedControl("format_ack has an invalid generation");
    return;
  }

  rc::control::StreamConfig applied;
  bool matches = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    matches = pendingStreamConfig_.has_value() &&
              metrics_.pendingFormatGeneration == generation && !resettingConsumer_;
    if (matches) {
      applied = *pendingStreamConfig_;
      resetQueueLocked(true);
      streamConfig_ = applied;
      pendingStreamConfig_.reset();
      metrics_.streamGeneration = generation;
      metrics_.pendingFormatGeneration = 0;
      metrics_.waitingForKeyframe = true;
      resettingConsumer_ = true;
      receivedVideo_ = false;
      lastActivity_ = clock_->now();
      streamingAt_ = lastActivity_;
      lastVideoAt_ = lastActivity_;
    }
  }
  if (!matches) {
    protocolFailure(connection, "format_ack does not match the pending generation");
    return;
  }

  resetConsumer(applied, generation);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    resettingConsumer_ = false;
  }
  notify("stream.reset", "generation " + std::to_string(generation));
  const HRESULT keyframeHr = sendControl(connection, rc::control::requestKeyframe());
  if (FAILED(keyframeHr)) protocolFailure(connection, "could not request a recovery keyframe");
}

void SessionController::handleFormatReject(rcnet::Connection& connection,
                                           const rc::control::Message& message) {
  rc::control::FormatReject rejection;
  if (!rc::control::parseFormatReject(message, rejection)) {
    malformedControl("format_reject has an invalid v1 shape");
    return;
  }

  bool matches = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    matches = pendingStreamConfig_.has_value() &&
              metrics_.pendingFormatGeneration == rejection.generation &&
              !resettingConsumer_;
    if (matches) {
      // The committed decoder/configuration was never changed. Drop anything that
      // raced the transition, reopen generation N-1, and require a recovery point.
      resetQueueLocked(true);
      pendingStreamConfig_.reset();
      metrics_.pendingFormatGeneration = 0;
      metrics_.waitingForKeyframe = true;
      receivedVideo_ = false;
      lastActivity_ = clock_->now();
      streamingAt_ = lastActivity_;
      lastVideoAt_ = lastActivity_;
    }
  }
  if (!matches) {
    protocolFailure(connection, "format_reject does not match the pending generation");
    return;
  }

  notify("stream.format_rejected",
         "generation " + std::to_string(rejection.generation) + ": " + rejection.code);
  const HRESULT keyframeHr = sendControl(connection, rc::control::requestKeyframe());
  if (FAILED(keyframeHr)) protocolFailure(connection, "could not request a recovery keyframe");
}

void SessionController::handleHello(rcnet::Connection& connection,
                                    const rc::control::Message& message) {
  rc::control::Hello parsed;
  if (!rc::control::parseHello(message, parsed)) {
    notify("handshake.failure", "hello has an invalid v1 identity shape");
    protocolFailure(connection, "invalid hello");
    return;
  }
  if (parsed.version != rc::control::kProtocolVersion) {
    notify("handshake.failure", "peer protocol must be exactly v1");
    protocolFailure(connection, "unsupported protocol version");
    return;
  }
  bool trusted = false;
  bool paired = false;
  bool allowUnauthenticated = false;
  std::optional<rcsecurity::AuthenticationChallenge> challenge;
  if (trustPolicy_ != nullptr) {
    allowUnauthenticated = trustPolicy_->allowsUnauthenticated();
    trusted = trustPolicy_->trusted(parsed);
    // Trust granted with no authenticated identity behind it. `paired` stays a statement
    // about a stored pairing record, so it must not be conflated with this.
    paired = trusted && !allowUnauthenticated;
  } else if (sessionSecurity_ != nullptr) {
    rcsecurity::AuthenticationChallenge generated;
    const rcsecurity::Error securityError = sessionSecurity_->beginAuthentication(
        sourceAddress(connection), parsed.deviceId, unixNow(), generated);
    if (securityError == rcsecurity::Error::None) {
      paired = true;
      challenge = generated;
    } else if (securityError != rcsecurity::Error::NotFound) {
      notify("security.authentication_failed", rcsecurity::errorText(securityError));
      protocolFailure(connection, "could not begin authenticated session");
      return;
    }
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    hello_ = parsed;
    metrics_.trusted = trusted;
    metrics_.state = trusted ? State::Ready : State::AwaitingTrust;
    const IClock::TimePoint now = clock_->now();
    lastActivity_ = now;
    trustAt_ = now;
    if (trusted) readyAt_ = now;
  }

  const HRESULT infoHr =
      sendControl(connection, rc::control::serverInfo(config_.serverName, config_.serviceId,
                                                      paired, allowUnauthenticated,
                                                      {"h264", "hevc"}));
  if (FAILED(infoHr)) {
    notify("handshake.failure", "could not send server_info");
    protocolFailure(connection, "server_info send failed");
    return;
  }
  if (challenge.has_value()) {
    rc::control::AuthChallenge wireChallenge;
    wireChallenge.serverNonce = challenge->serverNonce;
    wireChallenge.expiresUnixSeconds = challenge->expiresUnixSeconds;
    const HRESULT challengeHr = sendControl(connection,
                                             rc::control::authChallenge(wireChallenge));
    if (FAILED(challengeHr)) {
      protocolFailure(connection, "authentication challenge send failed");
      return;
    }
  } else if (trusted) {
    const HRESULT readyHr = sendControl(connection, rc::control::ready(streamConfig_));
    if (FAILED(readyHr)) {
      notify("handshake.failure", "could not send ready");
      protocolFailure(connection, "ready send failed");
      return;
    }
  }
  // Emitted before session.state so the UI settles on the downgrade wording rather than
  // briefly claiming an authenticated session it never had.
  if (trusted && allowUnauthenticated) {
    notify("security.unauthenticated", parsed.deviceName + " (" + parsed.deviceId + ")");
  }
  notify("session.state", stateName(trusted ? State::Ready : State::AwaitingTrust));
}

void SessionController::handleAuthResponse(rcnet::Connection& connection,
                                           const rc::control::Message& message) {
  rc::control::AuthResponse response;
  if (!rc::control::parseAuthResponse(message, response)) {
    malformedControl("auth_response has an invalid v1 shape");
    return;
  }
  std::string deviceId;
  bool hasIdentity = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (hello_.has_value()) {
      deviceId = hello_->deviceId;
      hasIdentity = true;
    }
  }
  if (!hasIdentity) {
    protocolFailure(connection, "authentication has no peer identity");
    return;
  }
  rcsecurity::AuthenticationResult authenticated;
  const rcsecurity::Error error = sessionSecurity_->finishAuthentication(
      sourceAddress(connection), deviceId, response.clientNonce, response.clientProof,
      unixNow(), authenticated);
  if (error != rcsecurity::Error::None || !authenticated.protector) {
    notify("security.authentication_failed", rcsecurity::errorText(error));
    protocolFailure(connection, "session authentication proof rejected");
    return;
  }

  rc::control::AuthConfirm confirmation;
  confirmation.serverProof = authenticated.serverProof;
  confirmation.sessionExpiresUnixSeconds =
      authenticated.protector->expiresUnixSeconds();
  // AUTH_CONFIRM is the final cleartext handshake message. Both peers install the
  // protector immediately after it; READY and every later control are enveloped.
  const HRESULT confirmHr = sendControl(connection, rc::control::authConfirm(confirmation));
  if (FAILED(confirmHr)) {
    protocolFailure(connection, "authentication confirmation send failed");
    return;
  }
  protector_.store(authenticated.protector);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    metrics_.trusted = true;
    metrics_.state = State::Ready;
    readyAt_ = clock_->now();
    lastActivity_ = readyAt_;
  }
  const HRESULT readyHr = sendControl(connection, rc::control::ready(streamConfig_));
  if (FAILED(readyHr)) {
    protocolFailure(connection, "authenticated ready send failed");
    return;
  }
  notify("security.authenticated", deviceId);
  notify("session.state", stateName(State::Ready));
}

void SessionController::handleVideo(rcnet::Connection& connection,
                                    const rc::wire::Frame& frame) {
  bool trusted = false;
  bool streaming = false;
  bool transitionPending = false;
  rc::control::Codec codec = rc::control::Codec::H264;
  uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    trusted = metrics_.trusted;
    streaming = metrics_.state == State::Streaming;
    transitionPending = pendingStreamConfig_.has_value() || resettingConsumer_;
    codec = streamConfig_.codec;
    generation = metrics_.streamGeneration;
  }
  if (!trusted) {
    notify("video.unauthenticated", "ignored");
    return;
  }
  if (!streaming || transitionPending) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++metrics_.droppedFrames;
    }
    notify("video.out_of_state", transitionPending ? "format transition pending"
                                                    : "stream_start not received");
    return;
  }

  const rc::annexb::Codec annexbCodec = codec == rc::control::Codec::Hevc
                                            ? rc::annexb::Codec::Hevc
                                            : rc::annexb::Codec::H264;
  const rc::annexb::AccessUnitReport report = rc::annexb::inspect(frame.payload, annexbCodec);
  const bool flagMatchesBitstream = frame.isKeyframe() == report.hasKeyframeSlice;
  if (!report.isAnnexB || !report.hasVideoSlice || !flagMatchesBitstream ||
      (frame.isKeyframe() && !report.isSelfContainedKeyframe)) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++metrics_.droppedFrames;
      resetQueueLocked(true);
    }
    std::string reason = "payload is not Annex-B";
    if (report.isAnnexB && !report.hasVideoSlice) {
      reason = "access unit has no video slice";
    } else if (report.isAnnexB && !flagMatchesBitstream) {
      reason = "keyframe flag disagrees with the bitstream";
    } else if (report.isAnnexB) {
      reason = "keyframe is not independently decodable";
    }
    notify("video.invalid", reason);
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
    const IClock::TimePoint now = clock_->now();
    lastActivity_ = now;
    lastVideoAt_ = now;
    receivedVideo_ = true;
    if (queue_.size() >= config_.maxQueuedAccessUnits ||
        metrics_.queueBytes + frame.payload.size() > config_.maxQueuedBytes) {
      resetQueueLocked(true);
      ++metrics_.droppedFrames;
      overflow = true;
    } else {
      EncodedAccessUnit unit;
      unit.bytes = frame.payload;
      unit.codec = codec;
      unit.keyframe = frame.isKeyframe();
      unit.ptsMicros = frame.ptsMicros;
      unit.generation = generation;
      queue_.push_back({nextQueueToken_++, std::move(unit)});
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
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (draining_) return;
    draining_ = true;
  }
  for (;;) {
    QueuedAccessUnit queued;
    IEncodedConsumer* consumer = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (queue_.empty() || consumer_ == nullptr || resettingConsumer_) {
        draining_ = false;
        return;
      }
      queued = queue_.front();
      consumer = consumer_;
    }
    bool consumed = false;
    {
      std::lock_guard<std::mutex> callbackLock(consumerMutex_);
      consumed = consumer->consume(queued.unit);
    }
    if (!consumed) {
      std::lock_guard<std::mutex> lock(mutex_);
      draining_ = false;
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      // A format transition may have flushed the queue while consume() was in flight.
      // Remove only the exact unit just consumed, never the new generation's front.
      if (!queue_.empty() && queue_.front().token == queued.token) {
        metrics_.queueBytes -= queue_.front().unit.bytes.size();
        queue_.pop_front();
      }
      metrics_.queueDepth = queue_.size();
    }
  }
}

void SessionController::resetQueue(bool countDrops) {
  std::lock_guard<std::mutex> lock(mutex_);
  resetQueueLocked(countDrops);
}

void SessionController::resetQueueLocked(bool countDrops) {
  if (countDrops) metrics_.droppedFrames += queue_.size();
  queue_.clear();
  metrics_.queueDepth = 0;
  metrics_.queueBytes = 0;
  metrics_.waitingForKeyframe = true;
}

void SessionController::onDisconnected(rcnet::Connection&, HRESULT reason) {
  stopWatchdog();
  protector_.store(nullptr);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    connection_ = nullptr;
    metrics_.state = State::Disconnected;
    metrics_.trusted = false;
    hello_.reset();
    caps_.reset();
    pendingStreamConfig_.reset();
    metrics_.pendingFormatGeneration = 0;
    metrics_.streamGeneration = 0;
    resettingConsumer_ = false;
    receivedVideo_ = false;
    resetQueueLocked(true);
  }
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

  HRESULT hr = S_OK;
  uint64_t generation = 0;
  {
    // Record the pending generation before putting it on the wire. Otherwise a fast
    // loopback peer can acknowledge it while send() is returning and the receive
    // thread would see an unsolicited acknowledgement.
    std::lock_guard<std::mutex> lock(mutex_);
    if (connection_ == nullptr || !metrics_.trusted || metrics_.state != State::Streaming) {
      return HRESULT_FROM_WIN32(ERROR_NOT_READY);
    }
    if (pendingStreamConfig_.has_value() || resettingConsumer_) {
      return HRESULT_FROM_WIN32(ERROR_BUSY);
    }
    generation = nextFormatGeneration_++;
    pendingStreamConfig_ = config;
    metrics_.pendingFormatGeneration = generation;
    pendingFormatAt_ = clock_->now();
    resetQueueLocked(true);

    hr = sendControl(*connection_, rc::control::setFormat(config, generation));
    if (FAILED(hr)) {
      pendingStreamConfig_.reset();
      metrics_.pendingFormatGeneration = 0;
    }
  }
  if (SUCCEEDED(hr)) notify("stream.format_pending", "generation " + std::to_string(generation));
  return hr;
}

HRESULT SessionController::sendStats(const rc::control::Stats& stats) {
  return sendControl(rc::control::stats(stats), rc::wire::Channel::Stats);
}

HRESULT SessionController::requestKeyframe() {
  return sendControl(rc::control::requestKeyframe());
}

void SessionController::setConsumer(IEncodedConsumer* consumer) {
  bool reset = false;
  rc::control::StreamConfig config;
  uint64_t generation = 0;
  {
    // Wait for an in-flight callback before replacing the non-owning pointer; callers
    // may release the old consumer as soon as this function returns.
    std::lock_guard<std::mutex> callbackLock(consumerMutex_);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      consumer_ = consumer;
      reset = consumer != nullptr && metrics_.state == State::Streaming &&
              !pendingStreamConfig_.has_value() && !resettingConsumer_;
      if (reset) {
        config = streamConfig_;
        generation = metrics_.streamGeneration;
        resettingConsumer_ = true;
      }
    }
  }
  if (reset) {
    resetConsumer(config, generation);
    std::lock_guard<std::mutex> lock(mutex_);
    resettingConsumer_ = false;
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
        } else if (metrics_.state == State::AwaitingTrust &&
                   now - trustAt_ > config_.trustTimeout) {
          expired = connection_;
          reason = "trust timeout";
        } else if (pendingStreamConfig_.has_value() &&
                   now - pendingFormatAt_ > config_.formatAckTimeout) {
          expired = connection_;
          reason = "format acknowledgement timeout";
        } else if (metrics_.state == State::Ready &&
                   now - readyAt_ > config_.progressTimeout) {
          expired = connection_;
          reason = "stream progress timeout";
        } else if (metrics_.state == State::Streaming &&
                   now - (receivedVideo_ ? lastVideoAt_ : streamingAt_) >
                       config_.videoTimeout) {
          expired = connection_;
          reason = receivedVideo_ ? "video idle timeout" : "first video timeout";
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
