#ifndef RCAPP_FRAME_PRODUCER_H
#define RCAPP_FRAME_PRODUCER_H

#include <windows.h>

#include <QObject>
#include <QString>
#include <thread>

namespace rcapp {

class FrameProducer final : public QObject {
  Q_OBJECT
  Q_PROPERTY(ConnectionState connectionState READ connectionState NOTIFY connectionStateChanged)
  Q_PROPERTY(QString connectionLabel READ connectionLabel NOTIFY connectionStateChanged)
  Q_PROPERTY(QString connectionDetail READ connectionDetail NOTIFY connectionStateChanged)
  Q_PROPERTY(bool publishing READ publishing NOTIFY connectionStateChanged)
  Q_PROPERTY(QString outputFormat READ outputFormat CONSTANT)
  Q_PROPERTY(QString outputResolution READ outputResolution CONSTANT)
  Q_PROPERTY(QString outputFrameRate READ outputFrameRate CONSTANT)

 public:
  enum class ConnectionState {
    WaitingForCameraConsumer,
    ConnectedPublishing,
    ProducerConflict,
    ActualFailure,
  };
  Q_ENUM(ConnectionState)

  explicit FrameProducer(QObject* parent = nullptr);
  ~FrameProducer() override;

  FrameProducer(const FrameProducer&) = delete;
  FrameProducer& operator=(const FrameProducer&) = delete;

  ConnectionState connectionState() const { return connectionState_; }
  QString connectionLabel() const;
  QString connectionDetail() const { return connectionDetail_; }
  bool publishing() const { return connectionState_ == ConnectionState::ConnectedPublishing; }

  QString outputFormat() const { return QStringLiteral("NV12"); }
  QString outputResolution() const { return QStringLiteral("1920 x 1080"); }
  QString outputFrameRate() const { return QStringLiteral("30 fps"); }

  void start();
  void stop();
  void setProducerConflict();
  void setStartupFailure(const QString& detail);

 signals:
  void connectionStateChanged();

 private:
  void run(std::stop_token stopToken);
  void postState(ConnectionState state, QString detail);

  ConnectionState connectionState_ = ConnectionState::WaitingForCameraConsumer;
  QString connectionDetail_ =
      QStringLiteral("Select the RemoteCam virtual camera in a camera application.");
  HANDLE stopEvent_ = nullptr;
  std::jthread worker_;
};

}  // namespace rcapp

#endif  // RCAPP_FRAME_PRODUCER_H
