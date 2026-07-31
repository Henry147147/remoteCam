#ifndef RCAPP_LIVE_MEDIA_PIPELINE_H
#define RCAPP_LIVE_MEDIA_PIPELINE_H

#include <QObject>
#include <QElapsedTimer>
#include <QString>
#include <QTimer>

#include <condition_variable>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "rc/transform.h"
#include "rcbackend/session_controller.h"
#include "rcplatform/mp4_recorder.h"

namespace rcapp {

class PreviewProvider;

// Production encoded-video consumer. The network callback only copies into a bounded
// queue; FFmpeg, D3D11 transform, readback and the camera ring run off the socket
// thread. The synthetic FrameProducer is compiled into the E2E host only.
class LiveMediaPipeline final : public QObject, public rcbackend::IEncodedConsumer {
  Q_OBJECT
  Q_PROPERTY(ConnectionState connectionState READ connectionState NOTIFY outputChanged)
  Q_PROPERTY(QString connectionLabel READ connectionLabel NOTIFY outputChanged)
  Q_PROPERTY(QString connectionDetail READ connectionDetail NOTIFY outputChanged)
  Q_PROPERTY(bool publishing READ publishing NOTIFY outputChanged)
  Q_PROPERTY(QString outputFormat READ outputFormat CONSTANT)
  Q_PROPERTY(QString outputResolution READ outputResolution NOTIFY outputChanged)
  Q_PROPERTY(QString outputFrameRate READ outputFrameRate CONSTANT)
  Q_PROPERTY(double rotationDeg READ rotationDeg WRITE setRotationDeg NOTIFY transformChanged)
  Q_PROPERTY(int fitMode READ fitMode WRITE setFitMode NOTIFY transformChanged)
  Q_PROPERTY(bool flipH READ flipH WRITE setFlipH NOTIFY transformChanged)
  Q_PROPERTY(bool flipV READ flipV WRITE setFlipV NOTIFY transformChanged)
  Q_PROPERTY(double zoom READ zoom WRITE setZoom NOTIFY transformChanged)
  Q_PROPERTY(QString previewSource READ previewSource NOTIFY previewChanged)
  Q_PROPERTY(QString screenshotStatus READ screenshotStatus NOTIFY screenshotChanged)
  Q_PROPERTY(QString lastScreenshotPath READ lastScreenshotPath NOTIFY screenshotChanged)
  Q_PROPERTY(bool recording READ recording NOTIFY recordingChanged)
  Q_PROPERTY(bool recordingCanToggle READ recordingCanToggle NOTIFY recordingChanged)
  Q_PROPERTY(QString recordingStatus READ recordingStatus NOTIFY recordingChanged)
  Q_PROPERTY(QString recordingDuration READ recordingDuration NOTIFY recordingChanged)
  Q_PROPERTY(QString recordingPath READ recordingPath NOTIFY recordingChanged)
  Q_PROPERTY(qulonglong recordingDroppedFrames READ recordingDroppedFrames
                 NOTIFY recordingChanged)

 public:
  enum class ConnectionState {
    WaitingForCameraConsumer,
    ConnectedPublishing,
    ProducerConflict,
    ActualFailure,
  };
  Q_ENUM(ConnectionState)

  explicit LiveMediaPipeline(QObject* parent = nullptr);
  ~LiveMediaPipeline() override;

  LiveMediaPipeline(const LiveMediaPipeline&) = delete;
  LiveMediaPipeline& operator=(const LiveMediaPipeline&) = delete;

  ConnectionState connectionState() const { return connectionState_; }
  QString connectionLabel() const { return connectionLabel_; }
  QString connectionDetail() const { return connectionDetail_; }
  bool publishing() const { return connectionState_ == ConnectionState::ConnectedPublishing; }
  QString outputFormat() const { return QStringLiteral("NV12"); }
  QString outputResolution() const { return outputResolution_; }
  QString outputFrameRate() const { return QStringLiteral("Consumer-selected"); }
  QString previewSource() const;
  QString screenshotStatus() const { return screenshotStatus_; }
  QString lastScreenshotPath() const { return lastScreenshotPath_; }
  bool recording() const;
  bool recordingCanToggle() const;
  QString recordingStatus() const { return recordingStatus_; }
  QString recordingDuration() const;
  QString recordingPath() const { return recordingPath_; }
  qulonglong recordingDroppedFrames() const {
    return static_cast<qulonglong>(recordingDroppedFrames_);
  }

  double rotationDeg() const;
  int fitMode() const;
  bool flipH() const;
  bool flipV() const;
  double zoom() const;

  void start();
  void stop();
  void setProducerConflict();
  void setStartupFailure(const QString& detail);
  void setKeyframeRequester(std::function<void()> requester);
  void setPreviewProvider(PreviewProvider* provider);
  bool consume(const rcbackend::EncodedAccessUnit& unit) override;
  void reset(const rc::control::StreamConfig& config, uint64_t generation) override;

 public slots:
  void setRotationDeg(double value);
  void setFitMode(int value);
  void setFlipH(bool value);
  void setFlipV(bool value);
  void setZoom(double value);
  void resetTransform();
  Q_INVOKABLE void takeScreenshot();
  Q_INVOKABLE void startRecording();
  Q_INVOKABLE void stopRecording();
  Q_INVOKABLE void toggleRecording();

 signals:
  void outputChanged();
  void transformChanged();
  void previewChanged();
  void screenshotChanged();
  void recordingChanged();

 private:
  void run(std::stop_token stopToken);
  void postFailure(QString detail);
  void requestRecoveryKeyframe();
  void applyRecorderSnapshot(const rcplatform::Mp4RecorderSnapshot& snapshot);
  void stopRecordingForFrameError(HRESULT error);
  void updateRecordingDuration();
  rc::TransformParams transformFor(uint32_t sourceWidth, uint32_t sourceHeight,
                                   uint32_t outputWidth, uint32_t outputHeight) const;
  void persistTransform() const;

  mutable std::mutex transformMutex_;
  rc::TransformParams transform_;

  mutable std::mutex queueMutex_;
  std::condition_variable_any queueWake_;
  std::deque<rcbackend::EncodedAccessUnit> queue_;
  std::optional<std::pair<rc::control::StreamConfig, uint64_t>> pendingReset_;
  size_t queueBytes_ = 0;
  bool waitingForKeyframe_ = true;
  std::function<void()> keyframeRequester_;
  std::jthread worker_;

  ConnectionState connectionState_ = ConnectionState::WaitingForCameraConsumer;
  QString connectionLabel_ = QStringLiteral("Waiting for camera consumer");
  QString connectionDetail_ =
      QStringLiteral("Open RemoteCam in a camera application, then connect a paired iPhone.");
  QString outputResolution_ = QStringLiteral("1920 x 1080");
  QString screenshotStatus_ = QStringLiteral("Ready");
  QString lastScreenshotPath_;
  rcplatform::Mp4RecorderState recorderState_ = rcplatform::Mp4RecorderState::Idle;
  QString recordingStatus_ = QStringLiteral("Ready to record");
  QString recordingPath_;
  uint64_t recordingDroppedFrames_ = 0;
  qint64 recordingDurationSeconds_ = 0;
  QElapsedTimer recordingElapsed_;
  QTimer recordingTimer_;
  std::atomic<PreviewProvider*> previewProvider_{nullptr};
  std::atomic<uint64_t> latestPreviewGeometry_{0};
  std::atomic<uint32_t> streamFrameRate_{30};
  std::atomic<bool> recordingFrameErrorQueued_{false};
  uint64_t previewRevision_ = 0;

  class PipelineImpl;
  std::unique_ptr<PipelineImpl> pipeline_;
};

}  // namespace rcapp

#endif  // RCAPP_LIVE_MEDIA_PIPELINE_H
