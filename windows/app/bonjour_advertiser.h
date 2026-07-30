#ifndef RCAPP_BONJOUR_ADVERTISER_H
#define RCAPP_BONJOUR_ADVERTISER_H

#include <windows.h>
#include <windns.h>

#include <QObject>
#include <QString>

namespace rcapp {

class BonjourAdvertiser final : public QObject {
  Q_OBJECT
  Q_PROPERTY(AdvertisementState state READ state NOTIFY stateChanged)
  Q_PROPERTY(QString statusLabel READ statusLabel NOTIFY stateChanged)
  Q_PROPERTY(QString statusDetail READ statusDetail NOTIFY stateChanged)
  Q_PROPERTY(QString computerName READ computerName CONSTANT)
  Q_PROPERTY(QString serviceID READ serviceID CONSTANT)
  Q_PROPERTY(quint16 port READ port CONSTANT)

 public:
  enum class AdvertisementState {
    Starting,
    Advertising,
    Failed,
    WaitingForReceiver,
    TestLoopback,
  };
  Q_ENUM(AdvertisementState)

  static constexpr quint16 kDefaultPort = 7890;

  explicit BonjourAdvertiser(QObject* parent = nullptr);
  ~BonjourAdvertiser() override;

  BonjourAdvertiser(const BonjourAdvertiser&) = delete;
  BonjourAdvertiser& operator=(const BonjourAdvertiser&) = delete;

  AdvertisementState state() const { return state_; }
  QString statusLabel() const;
  QString statusDetail() const { return statusDetail_; }
  QString computerName() const { return computerName_; }
  QString serviceID() const { return serviceID_; }
  quint16 port() const { return kDefaultPort; }

  void start();
  // Keeps the discovery card truthful when bind/listen fails. The caller must not
  // call start() after this without first constructing a new receiver.
  void setReceiverFailure(const QString& detail);
  // Native desktop tests bind only 127.0.0.1 so they never prompt for firewall access
  // or publish an unreachable LAN service. This is a truthful UI state, not a fake
  // successful Bonjour registration.
  void setTestLoopback();

 signals:
  void stateChanged();

 private:
  struct RegistrationContext;

  static void WINAPI registrationComplete(DWORD status, void* queryContext,
                                           PDNS_SERVICE_INSTANCE instance);
  void finishRegistration(DWORD status);
  void setFailure(QString detail);

  AdvertisementState state_ = AdvertisementState::WaitingForReceiver;
  QString statusDetail_ = QStringLiteral(
      "Automatic discovery will start after the Windows TCP receiver is listening.");
  QString computerName_;
  QString serviceID_;
  DNS_SERVICE_REGISTER_REQUEST request_{};
  DNS_SERVICE_CANCEL cancel_{};
  PDNS_SERVICE_INSTANCE instance_ = nullptr;
  RegistrationContext* context_ = nullptr;
  bool callbackPending_ = false;
};

}  // namespace rcapp

#endif  // RCAPP_BONJOUR_ADVERTISER_H
