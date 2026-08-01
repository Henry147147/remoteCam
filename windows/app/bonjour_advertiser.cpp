#include "bonjour_advertiser.h"

#include <QMetaObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>

#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <utility>

#include "rcwin/hr.h"

namespace rcapp {
namespace {

constexpr wchar_t kProtocolVersion[] = L"1";
constexpr wchar_t kCapabilities[] = L"h264,hevc";

QString loadComputerName() {
  std::array<wchar_t, MAX_COMPUTERNAME_LENGTH + 1> buffer{};
  DWORD size = static_cast<DWORD>(buffer.size());
  if (::GetComputerNameW(buffer.data(), &size) && size > 0) {
    return QString::fromWCharArray(buffer.data(), static_cast<qsizetype>(size));
  }
  return QStringLiteral("Windows PC");
}

// The bare DWORD that DnsServiceRegister returns is the only clue anyone gets when
// discovery silently does not work, and it was previously printed as a decimal number.
QString registrationError(DWORD status) {
  return QStringLiteral("Windows DNS-SD registration failed: %1 (0x%2).")
      .arg(QString::fromStdWString(rcwin::hrMessage(HRESULT_FROM_WIN32(status))))
      .arg(status, 8, 16, QLatin1Char('0'));
}

// DnsServiceDeRegister is asynchronous, so both the request and the instance it points
// at must outlive the call -- the same reason start() keeps request_ and instance_ as
// members. The completion callback owns this and frees it.
struct PendingDeregistration {
  DNS_SERVICE_REGISTER_REQUEST request{};
  PDNS_SERVICE_INSTANCE instance = nullptr;
};

QString loadOrCreateServiceID() {
  QSettings settings;
  const QRegularExpression validID(QStringLiteral("^[0-9a-f]{16}$"));
  QString id = settings.value(QStringLiteral("network/serviceID")).toString();
  if (validID.match(id).hasMatch()) return id;

  id = QStringLiteral("%1")
           .arg(QRandomGenerator::system()->generate64(), 16, 16, QLatin1Char('0'))
           .toLower();
  settings.setValue(QStringLiteral("network/serviceID"), id);
  return id;
}

}  // namespace

struct BonjourAdvertiser::RegistrationContext {
  std::atomic<BonjourAdvertiser*> owner;
  std::mutex ownerMutex;
  HANDLE callbackComplete = nullptr;
};

BonjourAdvertiser::BonjourAdvertiser(QObject* parent)
    : QObject(parent), computerName_(loadComputerName()), serviceID_(loadOrCreateServiceID()) {}

BonjourAdvertiser::~BonjourAdvertiser() {
  if (context_) {
    {
      std::lock_guard<std::mutex> lock(context_->ownerMutex);
      context_->owner.store(nullptr, std::memory_order_release);
    }
    if (callbackPending_) {
      ::DnsServiceRegisterCancel(&cancel_);
      // The context is callback-owned until Windows confirms completion. Avoid a
      // shutdown UAF even if the DNS client service is unhealthy and never answers.
      if (::WaitForSingleObject(context_->callbackComplete, 5000) != WAIT_OBJECT_0) {
        context_ = nullptr;  // intentional process-exit leak, safer than freeing in use
      }
    }
  }
  if (context_) {
    if (context_->callbackComplete) ::CloseHandle(context_->callbackComplete);
    delete context_;
  }
  if (instance_) ::DnsServiceFreeInstance(instance_);
}

QString BonjourAdvertiser::statusLabel() const {
  switch (state_) {
    case AdvertisementState::Starting:
      return QStringLiteral("Starting automatic discovery");
    case AdvertisementState::Advertising:
      return QStringLiteral("Visible to nearby iPhones");
    case AdvertisementState::Failed:
      return QStringLiteral("Automatic discovery unavailable");
    case AdvertisementState::WaitingForReceiver:
      return QStringLiteral("Waiting for network receiver");
    case AdvertisementState::TestLoopback:
      return QStringLiteral("Loopback desktop test");
  }
  return QStringLiteral("Automatic discovery unavailable");
}

void BonjourAdvertiser::start() {
  if (callbackPending_ || state_ == AdvertisementState::Advertising) return;

  // main.cpp only reaches here once the listener is bound, which is what makes a later
  // restart() safe to honour.
  receiverReady_ = true;
  state_ = AdvertisementState::Starting;
  statusDetail_ = QStringLiteral("Advertising %1 on TCP port %2...")
                      .arg(computerName_)
                      .arg(kDefaultPort);
  emit stateChanged();

  instanceName_ = QStringLiteral("%1._remotecam._tcp.local").arg(computerName_);
  // Must end in .local: see the note in rcnet::BonjourService::start. A bare computer
  // name registers successfully and browses fine from Windows, but no iPhone can resolve
  // it, which is indistinguishable from the PC never advertising at all.
  hostName_ = QStringLiteral("%1.local").arg(computerName_);
  const std::wstring instanceName = instanceName_.toStdWString();
  const std::wstring hostName = hostName_.toStdWString();
  const std::wstring displayName = computerName_.toStdWString();
  const std::wstring stableID = serviceID_.toStdWString();

  std::array<PCWSTR, 4> keys = {L"v", L"name", L"id", L"caps"};
  std::array<PCWSTR, 4> values = {kProtocolVersion, displayName.c_str(), stableID.c_str(),
                                  kCapabilities};

  instance_ = ::DnsServiceConstructInstance(
      instanceName.c_str(), hostName.c_str(), nullptr, nullptr, kDefaultPort, 0, 0,
      static_cast<DWORD>(keys.size()), keys.data(), values.data());
  if (!instance_) {
    setFailure(QStringLiteral("Windows could not create the Bonjour service record."));
    return;
  }

  context_ = new RegistrationContext;
  context_->owner.store(this, std::memory_order_release);
  context_->callbackComplete = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!context_->callbackComplete) {
    delete context_;
    context_ = nullptr;
    setFailure(QStringLiteral("Windows could not create the discovery callback guard."));
    ::DnsServiceFreeInstance(instance_);
    instance_ = nullptr;
    return;
  }
  request_ = {};
  request_.Version = DNS_QUERY_REQUEST_VERSION1;
  request_.InterfaceIndex = 0;
  request_.pServiceInstance = instance_;
  request_.pRegisterCompletionCallback = &BonjourAdvertiser::registrationComplete;
  request_.pQueryContext = context_;
  request_.unicastEnabled = FALSE;  // mDNS on the local link, not unicast DNS.

  callbackPending_ = true;
  cancel_ = {};
  const DWORD result = ::DnsServiceRegister(&request_, &cancel_);
  if (result != DNS_REQUEST_PENDING) {
    callbackPending_ = false;
    setFailure(registrationError(result));
    ::DnsServiceFreeInstance(instance_);
    instance_ = nullptr;
    ::CloseHandle(context_->callbackComplete);
    delete context_;
    context_ = nullptr;
  }
}

void BonjourAdvertiser::restart() {
  if (callbackPending_) return;
  // Never re-advertise a port nothing is listening on. Re-registering after a bind
  // failure produces the false-success discovery result main.cpp warns about: the phone
  // finds the PC and every connection times out.
  if (!receiverReady_) return;
  // Nothing but a de-register removes the old record; Windows otherwise keeps it until
  // the process exits, so a plain re-register would leave two instances on the link.
  if (state_ == AdvertisementState::Advertising) deregister();
  if (state_ == AdvertisementState::TestLoopback) return;
  state_ = AdvertisementState::WaitingForReceiver;
  start();
}

void BonjourAdvertiser::deregister() {
  const std::wstring instanceName = instanceName_.toStdWString();
  const std::wstring hostName = hostName_.toStdWString();
  auto* pending = new PendingDeregistration;
  pending->instance = ::DnsServiceConstructInstance(
      instanceName.c_str(), hostName.c_str(), nullptr, nullptr, kDefaultPort, 0, 0, 0,
      nullptr, nullptr);
  if (!pending->instance) {
    delete pending;
    return;
  }

  pending->request.Version = DNS_QUERY_REQUEST_VERSION1;
  pending->request.InterfaceIndex = 0;
  pending->request.pServiceInstance = pending->instance;
  // Best-effort withdrawal immediately followed by a fresh registration: we do not wait
  // for it, because blocking the UI thread on the DNS client would be worse than a stale
  // record the re-registration overwrites. Not waiting is exactly why the request and
  // instance are heap-owned by the callback rather than living on this stack frame.
  pending->request.pRegisterCompletionCallback = &BonjourAdvertiser::deregistrationComplete;
  pending->request.pQueryContext = pending;
  pending->request.unicastEnabled = FALSE;
  const DWORD status = ::DnsServiceDeRegister(&pending->request, nullptr);
  if (status != DNS_REQUEST_PENDING) {
    // The callback will not run, so nothing else will ever release this.
    if (status != ERROR_SUCCESS) {
      RC_WARN(L"DnsServiceDeRegister(%s) returned %s", instanceName.c_str(),
              rcwin::hrMessage(HRESULT_FROM_WIN32(status)).c_str());
    }
    ::DnsServiceFreeInstance(pending->instance);
    delete pending;
  }
  state_ = AdvertisementState::WaitingForReceiver;
}

void BonjourAdvertiser::setReceiverFailure(const QString& detail) {
  if (callbackPending_ || state_ == AdvertisementState::Advertising) return;
  receiverReady_ = false;
  setFailure(QStringLiteral("TCP port %1 is not listening: %2").arg(kDefaultPort).arg(detail));
}

void BonjourAdvertiser::setTestLoopback() {
  if (callbackPending_ || state_ == AdvertisementState::Advertising) return;
  state_ = AdvertisementState::TestLoopback;
  statusDetail_ = QStringLiteral(
      "The desktop verification host is listening on 127.0.0.1:%1 and is not advertised.")
                      .arg(kDefaultPort);
  emit stateChanged();
}

void WINAPI BonjourAdvertiser::deregistrationComplete(DWORD, void* queryContext,
                                                      PDNS_SERVICE_INSTANCE instance) {
  if (instance) ::DnsServiceFreeInstance(instance);
  auto* pending = static_cast<PendingDeregistration*>(queryContext);
  if (!pending) return;
  // The DNS client is done with both by the time it calls back, so this is the earliest
  // point at which either can be released.
  if (pending->instance) ::DnsServiceFreeInstance(pending->instance);
  delete pending;
}

void WINAPI BonjourAdvertiser::registrationComplete(DWORD status, void* queryContext,
                                                     PDNS_SERVICE_INSTANCE instance) {
  if (instance) ::DnsServiceFreeInstance(instance);

  auto* context = static_cast<RegistrationContext*>(queryContext);
  if (!context) return;
  {
    std::lock_guard<std::mutex> lock(context->ownerMutex);
    BonjourAdvertiser* owner = context->owner.load(std::memory_order_acquire);
    if (owner) {
      QMetaObject::invokeMethod(owner, [owner, status] { owner->finishRegistration(status); },
                                Qt::QueuedConnection);
    }
  }
  ::SetEvent(context->callbackComplete);
}

void BonjourAdvertiser::finishRegistration(DWORD status) {
  if (!callbackPending_) return;
  callbackPending_ = false;

  // Registration is complete and Windows owns its process-lifetime copy.
  if (instance_) {
    ::DnsServiceFreeInstance(instance_);
    instance_ = nullptr;
  }
  if (context_->callbackComplete) ::CloseHandle(context_->callbackComplete);
  delete context_;
  context_ = nullptr;

  if (status != ERROR_SUCCESS) {
    setFailure(registrationError(status));
    return;
  }

  state_ = AdvertisementState::Advertising;
  statusDetail_ = QStringLiteral("%1 is advertised as _remotecam._tcp on port %2. "
                                 "Manual connection remains available at the same port.")
                      .arg(computerName_)
                      .arg(kDefaultPort);
  // Log what was actually published, not just that publishing happened: a healthy-looking
  // registration with the wrong SRV target is the failure mode this component has.
  RC_LOG(L"Bonjour advertised %s -> %s:%u with id %s", instanceName_.toStdWString().c_str(),
         hostName_.toStdWString().c_str(), kDefaultPort,
         serviceID_.toStdWString().c_str());
  emit stateChanged();
}

void BonjourAdvertiser::setFailure(QString detail) {
  state_ = AdvertisementState::Failed;
  statusDetail_ = std::move(detail);
  RC_ERR(L"Bonjour advertisement failed: %s", statusDetail_.toStdWString().c_str());
  emit stateChanged();
}

}  // namespace rcapp
