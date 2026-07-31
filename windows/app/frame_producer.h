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
  Q_PROPERTY(QString previewSource READ previewSource CONSTANT)
  Q_PROPERTY(double rotationDeg READ rotationDeg WRITE setRotationDeg NOTIFY transformChanged)
  Q_PROPERTY(int fitMode READ fitMode WRITE setFitMode NOTIFY transformChanged)
  Q_PROPERTY(bool flipH READ flipH WRITE setFlipH NOTIFY transformChanged)
  Q_PROPERTY(bool flipV READ flipV WRITE setFlipV NOTIFY transformChanged)
  Q_PROPERTY(double zoom READ zoom WRITE setZoom NOTIFY transformChanged)
  Q_PROPERTY(QString screenshotStatus READ screenshotStatus CONSTANT)
  Q_PROPERTY(QString lastScreenshotPath READ lastScreenshotPath CONSTANT)
  Q_PROPERTY(bool recording READ recording CONSTANT)
  Q_PROPERTY(bool recordingCanToggle READ recordingCanToggle CONSTANT)
  Q_PROPERTY(QString recordingStatus READ recordingStatus CONSTANT)
  Q_PROPERTY(QString recordingDuration READ recordingDuration CONSTANT)
  Q_PROPERTY(QString recordingPath READ recordingPath CONSTANT)
  Q_PROPERTY(qulonglong recordingDroppedFrames READ recordingDroppedFrames CONSTANT)

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
  QString previewSource() const { return QString{}; }
  double rotationDeg() const { return 0.0; }
  int fitMode() const { return 0; }
  bool flipH() const { return false; }
  bool flipV() const { return false; }
  double zoom() const { return 1.0; }
  QString screenshotStatus() const { return QStringLiteral("Unavailable in E2E host"); }
  QString lastScreenshotPath() const { return QString{}; }
  bool recording() const { return false; }
  bool recordingCanToggle() const { return false; }
  QString recordingStatus() const { return QStringLiteral("Unavailable in E2E host"); }
  QString recordingDuration() const { return QStringLiteral("00:00"); }
  QString recordingPath() const { return QString{}; }
  qulonglong recordingDroppedFrames() const { return 0; }

  void start();
  void stop();
  void setProducerConflict();
  void setStartupFailure(const QString& detail);
  void setRotationDeg(double) {}
  void setFitMode(int) {}
  void setFlipH(bool) {}
  void setFlipV(bool) {}
  void setZoom(double) {}
  Q_INVOKABLE void resetTransform() {}
  Q_INVOKABLE void takeScreenshot() {}
  Q_INVOKABLE void startRecording() {}
  Q_INVOKABLE void stopRecording() {}
  Q_INVOKABLE void toggleRecording() {}

 signals:
  void connectionStateChanged();
  void transformChanged();

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
