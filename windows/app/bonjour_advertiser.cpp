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
  }
  return QStringLiteral("Automatic discovery unavailable");
}

void BonjourAdvertiser::start() {
  if (callbackPending_ || state_ == AdvertisementState::Advertising) return;

  state_ = AdvertisementState::Starting;
  statusDetail_ = QStringLiteral("Advertising %1 on TCP port %2...")
                      .arg(computerName_)
                      .arg(kDefaultPort);
  emit stateChanged();

  const std::wstring instanceName =
      QStringLiteral("%1._remotecam._tcp.local").arg(computerName_).toStdWString();
  const std::wstring hostName = computerName_.toStdWString();
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
    setFailure(QStringLiteral("Windows DNS-SD registration failed with error %1.").arg(result));
    ::DnsServiceFreeInstance(instance_);
    instance_ = nullptr;
    ::CloseHandle(context_->callbackComplete);
    delete context_;
    context_ = nullptr;
  }
}

void BonjourAdvertiser::setReceiverFailure(const QString& detail) {
  if (callbackPending_ || state_ == AdvertisementState::Advertising) return;
  setFailure(QStringLiteral("TCP port %1 is not listening: %2").arg(kDefaultPort).arg(detail));
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
    setFailure(QStringLiteral("Windows DNS-SD registration failed with error %1.").arg(status));
    return;
  }

  state_ = AdvertisementState::Advertising;
  statusDetail_ = QStringLiteral("%1 is advertised as _remotecam._tcp on port %2. "
                                 "Manual connection remains available at the same port.")
                      .arg(computerName_)
                      .arg(kDefaultPort);
  RC_LOG(L"Bonjour service advertised on TCP port %u with id %s", kDefaultPort,
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
