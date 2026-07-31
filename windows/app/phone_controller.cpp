#include "phone_controller.h"

#include <algorithm>
#include <cmath>

#include "rcwin/hr.h"

namespace rcapp {

PhoneController::PhoneController(rcbackend::SessionController& session, QObject* parent)
    : QObject(parent), session_(session) {
  pollTimer_.setInterval(500);
  connect(&pollTimer_, &QTimer::timeout, this, &PhoneController::pollSession);
  pollTimer_.start();

  controlTimer_.setSingleShot(true);
  controlTimer_.setInterval(75);
  connect(&controlTimer_, &QTimer::timeout, this, &PhoneController::flushControls);
}

void PhoneController::pollSession() {
  const rcbackend::Metrics metrics = session_.metrics();
  const bool nextEnabled = metrics.trusted &&
                           (metrics.state == rcbackend::State::Ready ||
                            metrics.state == rcbackend::State::Streaming);
  if (controlsEnabled_ != nextEnabled) {
    controlsEnabled_ = nextEnabled;
    if (!controlsEnabled_) {
      commandStatus_ = QStringLiteral("Connect a paired iPhone to enable controls.");
      emit commandStatusChanged();
    }
    emit stateChanged();
  }

  const std::optional<rc::control::Caps> caps = session_.peerCaps();
  if (!caps.has_value()) return;
  QStringList names;
  names.reserve(static_cast<qsizetype>(caps->cameras.size()));
  for (const rc::control::CameraDescriptor& camera : caps->cameras) {
    names.push_back(QString::fromStdString(camera.name));
  }
  if (names != cameraNames_) {
    cameraNames_ = names;
    cameras_ = caps->cameras;
    if (cameraIndex_ < 0 && !cameras_.empty()) cameraIndex_ = 0;
    emit capabilitiesChanged();
    emit selectionChanged();
  }
}

void PhoneController::setCameraIndex(int index) {
  if (index < 0 || index >= static_cast<int>(cameras_.size())) return;
  if (cameraIndex_ == index) return;
  cameraIndex_ = index;
  const auto& camera = cameras_[static_cast<size_t>(index)];
  report(session_.sendCamera(camera.lens, camera.position), QStringLiteral("Camera change"));
  emit selectionChanged();
}

void PhoneController::setCodec(const QString& codec) {
  const QString normalized = codec.toLower();
  if (normalized != QStringLiteral("h264") && normalized != QStringLiteral("hevc")) return;
  if (codec_ == normalized) return;
  codec_ = normalized;
  emit selectionChanged();
}

void PhoneController::setResolution(const QString& resolution) {
  if (resolution_ == resolution) return;
  const QStringList parts = resolution.split(QLatin1Char('x'));
  if (parts.size() != 2) return;
  bool widthOk = false;
  bool heightOk = false;
  const uint32_t width = parts[0].toUInt(&widthOk);
  const uint32_t height = parts[1].toUInt(&heightOk);
  if (!widthOk || !heightOk || width < 320 || height < 240 ||
      width > 3840 || height > 2160 || (width & 1u) != 0 || (height & 1u) != 0) {
    return;
  }
  resolution_ = resolution;
  emit selectionChanged();
}

void PhoneController::setFrameRate(int frameRate) {
  if (frameRate != 30 && frameRate != 60) return;
  if (frameRate_ == frameRate) return;
  frameRate_ = frameRate;
  emit selectionChanged();
}

void PhoneController::setPreviewEnabled(bool enabled) {
  if (previewEnabled_ == enabled) return;
  previewEnabled_ = enabled;
  report(session_.sendPreview(enabled), QStringLiteral("Phone preview"));
  emit selectionChanged();
}

void PhoneController::setPhoneZoom(double value) {
  if (!std::isfinite(value)) return;
  value = std::clamp(value, 1.0, 10.0);
  if (qFuzzyCompare(phoneZoom_, value)) return;
  phoneZoom_ = value;
  pendingControls_.zoom = value;
  scheduleControls();
  emit controlChanged();
}

void PhoneController::setFocus(double value) {
  if (!std::isfinite(value)) return;
  value = std::clamp(value, 0.0, 1.0);
  if (qFuzzyCompare(focus_, value)) return;
  focus_ = value;
  pendingControls_.focus = value;
  pendingControls_.focusMode = std::string("locked");
  scheduleControls();
  emit controlChanged();
}

void PhoneController::setExposureBias(double value) {
  if (!std::isfinite(value)) return;
  value = std::clamp(value, -4.0, 4.0);
  if (qFuzzyCompare(exposureBias_, value)) return;
  exposureBias_ = value;
  pendingControls_.ev = value;
  scheduleControls();
  emit controlChanged();
}

void PhoneController::setWhiteBalance(double value) {
  if (!std::isfinite(value)) return;
  value = std::clamp(value, 2500.0, 9000.0);
  if (qFuzzyCompare(whiteBalance_, value)) return;
  whiteBalance_ = value;
  pendingControls_.whiteBalance = value;
  pendingControls_.whiteBalanceMode = std::string("locked");
  scheduleControls();
  emit controlChanged();
}

void PhoneController::setTorch(bool enabled) {
  if (torch_ == enabled) return;
  torch_ = enabled;
  pendingControls_.torch = enabled;
  scheduleControls();
  emit controlChanged();
}

void PhoneController::setStabilization(bool enabled) {
  if (stabilization_ == enabled) return;
  stabilization_ = enabled;
  pendingControls_.stabilization = enabled;
  scheduleControls();
  emit controlChanged();
}

void PhoneController::applyFormat() {
  const QStringList parts = resolution_.split(QLatin1Char('x'));
  if (parts.size() != 2) return;
  rc::control::StreamConfig config;
  config.codec = codec_ == QStringLiteral("hevc") ? rc::control::Codec::Hevc
                                                   : rc::control::Codec::H264;
  config.width = parts[0].toUInt();
  config.height = parts[1].toUInt();
  config.fps = static_cast<uint32_t>(frameRate_);
  const uint64_t pixelsPerSecond =
      static_cast<uint64_t>(config.width) * config.height * config.fps;
  config.bitrate = static_cast<uint32_t>(std::clamp<uint64_t>(
      pixelsPerSecond / 7u, 1'000'000u, 35'000'000u));
  report(session_.sendFormat(config), QStringLiteral("Video format"));
}

void PhoneController::scheduleControls() {
  if (!controlTimer_.isActive()) controlTimer_.start();
}

void PhoneController::flushControls() {
  rc::control::CameraControls controls = std::move(pendingControls_);
  pendingControls_ = rc::control::CameraControls{};
  report(session_.sendControls(controls), QStringLiteral("Camera controls"));
}

void PhoneController::report(HRESULT result, const QString& action) {
  commandStatus_ = SUCCEEDED(result)
                       ? action + QStringLiteral(" sent")
                       : action + QStringLiteral(" failed: ") +
                             QString::fromStdWString(rcwin::hrMessage(result));
  emit commandStatusChanged();
}

}  // namespace rcapp
