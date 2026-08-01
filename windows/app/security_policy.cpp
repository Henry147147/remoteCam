#include "security_policy.h"

#include <QSettings>

#include "rcwin/hr.h"

namespace rcapp {

SecurityPolicy::SecurityPolicy(QObject* parent) : QObject(parent) {
  // Its own settings group: live_media_pipeline wipes the whole `transform` group on a
  // schema-version mismatch, which would silently re-open a PC the user had locked down.
  QSettings settings;
  const bool allow =
      settings.value(QStringLiteral("security/allowUnauthenticated"), true).toBool();
  allowUnauthenticated_.store(allow, std::memory_order_relaxed);
  RC_LOG(L"unauthenticated connections are %s", allow ? L"allowed" : L"refused");
}

bool SecurityPolicy::allowUnauthenticated() const {
  return allowUnauthenticated_.load(std::memory_order_relaxed);
}

void SecurityPolicy::setAllowUnauthenticated(bool allow) {
  if (allowUnauthenticated_.exchange(allow, std::memory_order_relaxed) == allow) return;
  QSettings().setValue(QStringLiteral("security/allowUnauthenticated"), allow);
  RC_LOG(L"unauthenticated connections are now %s", allow ? L"allowed" : L"refused");
  emit allowUnauthenticatedChanged();
}

bool SecurityPolicy::trusted(const rc::control::Hello& hello) {
  // Both halves, every time. The phone's flag alone is an unauthenticated peer asking to
  // be believed, and this PC's flag alone is an offer nobody accepted.
  return allowUnauthenticated() && hello.allowUnauthenticated;
}

}  // namespace rcapp
