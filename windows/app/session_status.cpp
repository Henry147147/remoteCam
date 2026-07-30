#include "session_status.h"

#include <QMetaObject>

#include <utility>

namespace rcapp {

void SessionStatus::onBackendEvent(const std::string& kind, const std::string& detail) {
  const QString qKind = QString::fromStdString(kind);
  const QString qDetail = QString::fromStdString(detail);
  QMetaObject::invokeMethod(
      this, [this, qKind, qDetail] { applyEvent(qKind, qDetail); }, Qt::QueuedConnection);
}

void SessionStatus::applyEvent(QString kind, QString detail) {
  if (kind == QStringLiteral("session.state")) {
    if (detail == QStringLiteral("awaiting_hello")) {
      stateLabel_ = QStringLiteral("iPhone connected");
      detail_ = QStringLiteral("Waiting for the phone identity handshake.");
      streaming_ = false;
    } else if (detail == QStringLiteral("awaiting_trust")) {
      stateLabel_ = QStringLiteral("Secure pairing required");
      detail_ = QStringLiteral(
          "The phone is identified, but authenticated pairing is not yet available.");
      streaming_ = false;
    } else if (detail == QStringLiteral("ready")) {
      stateLabel_ = QStringLiteral("Phone ready");
      detail_ = QStringLiteral("The authenticated test session is preparing video.");
      streaming_ = false;
    } else if (detail == QStringLiteral("streaming")) {
      stateLabel_ = QStringLiteral("Phone streaming");
      detail_ = QStringLiteral("Encoded phone video is arriving at the Windows backend.");
      streaming_ = true;
    } else if (detail.startsWith(QStringLiteral("disconnected"))) {
      stateLabel_ = QStringLiteral("Waiting for iPhone");
      detail_ = QStringLiteral("The phone disconnected from the receiver.");
      streaming_ = false;
    }
  } else if (kind == QStringLiteral("session.timeout")) {
    stateLabel_ = QStringLiteral("Phone timed out");
    detail_ = detail;
    streaming_ = false;
  } else if (kind == QStringLiteral("video.invalid") ||
             kind == QStringLiteral("video.backpressure")) {
    detail_ = kind + QStringLiteral(": ") + detail;
  }
  emit changed();
}

}  // namespace rcapp
