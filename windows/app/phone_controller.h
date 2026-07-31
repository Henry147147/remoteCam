#ifndef RCAPP_PHONE_CONTROLLER_H
#define RCAPP_PHONE_CONTROLLER_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <vector>

#include "rcbackend/session_controller.h"

namespace rcapp {

// Qt-facing phone command model. Continuous controls are coalesced before crossing
// the authenticated control channel; camera and format changes remain explicit.
class PhoneController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool controlsEnabled READ controlsEnabled NOTIFY stateChanged)
  Q_PROPERTY(QStringList cameraNames READ cameraNames NOTIFY capabilitiesChanged)
  Q_PROPERTY(int cameraIndex READ cameraIndex WRITE setCameraIndex NOTIFY selectionChanged)
  Q_PROPERTY(QString codec READ codec WRITE setCodec NOTIFY selectionChanged)
  Q_PROPERTY(QString resolution READ resolution WRITE setResolution NOTIFY selectionChanged)
  Q_PROPERTY(int frameRate READ frameRate WRITE setFrameRate NOTIFY selectionChanged)
  Q_PROPERTY(bool previewEnabled READ previewEnabled WRITE setPreviewEnabled
                 NOTIFY selectionChanged)
  Q_PROPERTY(double phoneZoom READ phoneZoom WRITE setPhoneZoom NOTIFY controlChanged)
  Q_PROPERTY(double focus READ focus WRITE setFocus NOTIFY controlChanged)
  Q_PROPERTY(double exposureBias READ exposureBias WRITE setExposureBias NOTIFY controlChanged)
  Q_PROPERTY(double whiteBalance READ whiteBalance WRITE setWhiteBalance
                 NOTIFY controlChanged)
  Q_PROPERTY(bool torch READ torch WRITE setTorch NOTIFY controlChanged)
  Q_PROPERTY(bool stabilization READ stabilization WRITE setStabilization
                 NOTIFY controlChanged)
  Q_PROPERTY(QString commandStatus READ commandStatus NOTIFY commandStatusChanged)

 public:
  explicit PhoneController(rcbackend::SessionController& session, QObject* parent = nullptr);

  bool controlsEnabled() const { return controlsEnabled_; }
  QStringList cameraNames() const { return cameraNames_; }
  int cameraIndex() const { return cameraIndex_; }
  QString codec() const { return codec_; }
  QString resolution() const { return resolution_; }
  int frameRate() const { return frameRate_; }
  bool previewEnabled() const { return previewEnabled_; }
  double phoneZoom() const { return phoneZoom_; }
  double focus() const { return focus_; }
  double exposureBias() const { return exposureBias_; }
  double whiteBalance() const { return whiteBalance_; }
  bool torch() const { return torch_; }
  bool stabilization() const { return stabilization_; }
  QString commandStatus() const { return commandStatus_; }

 public slots:
  void setCameraIndex(int index);
  void setCodec(const QString& codec);
  void setResolution(const QString& resolution);
  void setFrameRate(int frameRate);
  void setPreviewEnabled(bool enabled);
  void setPhoneZoom(double value);
  void setFocus(double value);
  void setExposureBias(double value);
  void setWhiteBalance(double value);
  void setTorch(bool enabled);
  void setStabilization(bool enabled);
  Q_INVOKABLE void applyFormat();

 signals:
  void stateChanged();
  void capabilitiesChanged();
  void selectionChanged();
  void controlChanged();
  void commandStatusChanged();

 private:
  void pollSession();
  void scheduleControls();
  void flushControls();
  void report(HRESULT result, const QString& action);

  rcbackend::SessionController& session_;
  QTimer pollTimer_;
  QTimer controlTimer_;
  bool controlsEnabled_ = false;
  QStringList cameraNames_;
  std::vector<rc::control::CameraDescriptor> cameras_;
  int cameraIndex_ = -1;
  QString codec_ = QStringLiteral("h264");
  QString resolution_ = QStringLiteral("1280x720");
  int frameRate_ = 30;
  bool previewEnabled_ = true;
  double phoneZoom_ = 1.0;
  double focus_ = 0.5;
  double exposureBias_ = 0.0;
  double whiteBalance_ = 5000.0;
  bool torch_ = false;
  bool stabilization_ = false;
  rc::control::CameraControls pendingControls_;
  QString commandStatus_ = QStringLiteral("Connect a paired iPhone to enable controls.");
};

}  // namespace rcapp

#endif  // RCAPP_PHONE_CONTROLLER_H
