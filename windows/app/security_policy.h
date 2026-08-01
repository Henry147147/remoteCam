#ifndef RCAPP_SECURITY_POLICY_H
#define RCAPP_SECURITY_POLICY_H

#include <QObject>

#include <atomic>

#include "rcbackend/session_controller.h"

namespace rcapp {

// The user-selectable pairing opt-out. It lives here rather than in rcwin-backend
// because a policy that grants trust without ISessionSecurity grants it without proof,
// and that library ships no such implementation.
//
// Both ends must opt in: this only reports the PC's half, and SessionController still
// requires the matching flag in the phone's hello. A phone therefore cannot authorize
// itself, and neither can a PC accept a phone that asked to be authenticated.
class SecurityPolicy final : public QObject, public rcbackend::ITrustPolicy {
  Q_OBJECT
  Q_PROPERTY(bool allowUnauthenticated READ allowUnauthenticated WRITE setAllowUnauthenticated
                 NOTIFY allowUnauthenticatedChanged)

 public:
  explicit SecurityPolicy(QObject* parent = nullptr);

  bool allowUnauthenticated() const;
  void setAllowUnauthenticated(bool allow);

  bool trusted(const rc::control::Hello& hello) override;
  bool allowsUnauthenticated() const override { return allowUnauthenticated(); }

 signals:
  void allowUnauthenticatedChanged();

 private:
  // trusted() runs on the listener thread while the setter runs on the Qt thread, so the
  // toggle takes effect on the next connection without a restart.
  std::atomic<bool> allowUnauthenticated_{true};
};

}  // namespace rcapp

#endif  // RCAPP_SECURITY_POLICY_H
