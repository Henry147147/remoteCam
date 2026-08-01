#ifndef RCAPP_SESSION_STATUS_H
#define RCAPP_SESSION_STATUS_H

#include <QObject>
#include <QString>

#include "rcbackend/session_controller.h"

namespace rcapp {

// Thread-safe Qt projection of backend checkpoints. Stable object/accessible names in
// QML consume these strings during native desktop automation, while the controller
// itself stays Qt-free and testable headlessly.
class SessionStatus final : public QObject, public rcbackend::IBackendObserver {
  Q_OBJECT
  Q_PROPERTY(QString stateLabel READ stateLabel NOTIFY changed)
  Q_PROPERTY(QString detail READ detail NOTIFY changed)
  Q_PROPERTY(bool streaming READ streaming NOTIFY changed)
  Q_PROPERTY(bool unauthenticated READ unauthenticated NOTIFY changed)

 public:
  explicit SessionStatus(QObject* parent = nullptr) : QObject(parent) {}

  QString stateLabel() const { return stateLabel_; }
  QString detail() const { return detail_; }
  bool streaming() const { return streaming_; }
  bool unauthenticated() const { return unauthenticated_; }

  void onBackendEvent(const std::string& kind, const std::string& detail) override;

 signals:
  void changed();

 private:
  void applyEvent(QString kind, QString detail);

  QString stateLabel_ = QStringLiteral("Waiting for iPhone");
  QString detail_ = QStringLiteral("No phone is connected to the receiver.");
  bool streaming_ = false;
  // Sticky for the life of the connection: the backend announces the skipped
  // authentication once, before ready, but the user needs to see it for the whole session.
  bool unauthenticated_ = false;
  QString peer_;
};

}  // namespace rcapp

#endif  // RCAPP_SESSION_STATUS_H
