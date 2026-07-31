#include "live_media_pipeline.h"

#include <QMetaObject>
#include <QSettings>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QStandardPaths>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <utility>

#include "rcplatform/d3d11_transform.h"
#include "rcplatform/frame_ring_sink.h"
#include "rcplatform/video_decoder.h"
#include "rcplatform/video_pipeline.h"
#include "rcwin/hr.h"
#include "preview_provider.h"

namespace rcapp {
namespace {

constexpr size_t kMaxQueuedUnits = 8;
constexpr size_t kMaxQueuedBytes = 20u * 1024u * 1024u;

rc::annexb::Codec platformCodec(rc::control::Codec codec) {
  return codec == rc::control::Codec::Hevc ? rc::annexb::Codec::Hevc
                                           : rc::annexb::Codec::H264;
}

QString decoderError(HRESULT error) {
  if (error == E_NOTIMPL) {
    return QStringLiteral(
        "This build does not contain the required FFmpeg decoder runtime. Reinstall "
        "RemoteCam using the production installer.");
  }
  return QStringLiteral("The video decoder could not start: %1")
      .arg(QString::fromStdWString(rcwin::hrMessage(error)));
}

}  // namespace

class LiveMediaPipeline::PipelineImpl {
 public:
  class AdaptiveTransform final : public rcplatform::IFrameTransform {
   public:
    explicit AdaptiveTransform(LiveMediaPipeline& owner) : owner_(owner) {}

    HRESULT apply(const rcplatform::TextureFrame& input, const rc::TransformParams&,
                  rcplatform::TextureFrame& output) override {
      const auto [width, height] = sink.desiredGeometry();
      const rc::TransformParams params =
          owner_.transformFor(input.width, input.height, width, height);
      return transform.apply(input, params, output);
    }

    LiveMediaPipeline& owner_;
    rcplatform::D3D11TransformPass transform;
    rcplatform::FrameRingSink sink;
  };

  explicit PipelineImpl(LiveMediaPipeline& owner)
      : adaptive(owner), recorder(rcplatform::createMp4Recorder()) {}

  void startScreenshots(LiveMediaPipeline& owner) {
    if (screenshotWorker.joinable()) return;
    screenshotWorker = std::jthread([this, &owner](std::stop_token stopToken) {
      while (!stopToken.stop_requested()) {
        std::pair<QImage, QString> job;
        {
          std::unique_lock<std::mutex> lock(screenshotMutex);
          screenshotWake.wait(lock, stopToken, [this] { return !screenshotJobs.empty(); });
          if (stopToken.stop_requested()) break;
          job = std::move(screenshotJobs.front());
          screenshotJobs.pop_front();
        }
        const QFileInfo file(job.second);
        const bool directoryReady = QDir().mkpath(file.absolutePath());
        const bool saved = directoryReady && job.first.save(job.second, "PNG");
        QMetaObject::invokeMethod(
            &owner,
            [&owner, path = job.second, saved] {
              owner.screenshotStatus_ =
                  saved ? QStringLiteral("Screenshot saved")
                        : QStringLiteral("Could not save screenshot");
              if (saved) owner.lastScreenshotPath_ = path;
              emit owner.screenshotChanged();
            },
            Qt::QueuedConnection);
      }
    });
  }

  void stopScreenshots() {
    if (!screenshotWorker.joinable()) return;
    screenshotWorker.request_stop();
    screenshotWake.notify_all();
    screenshotWorker.join();
    std::lock_guard<std::mutex> lock(screenshotMutex);
    screenshotJobs.clear();
  }

  HRESULT selectCodec(rc::annexb::Codec next) {
    if (codec == next && pipeline) return S_OK;
    pipeline.reset();
    decoder.reset();
    const HRESULT hr = rcplatform::createFfmpegD3D11Decoder(next, decoder);
    if (FAILED(hr)) return hr;
    pipeline = std::make_unique<rcplatform::VideoPipeline>(*decoder, adaptive, nullptr,
                                                           adaptive.sink);
    codec = next;
    return S_OK;
  }

  AdaptiveTransform adaptive;
  std::unique_ptr<rcplatform::IMp4Recorder> recorder;
  std::unique_ptr<rcplatform::IVideoDecoder> decoder;
  std::unique_ptr<rcplatform::VideoPipeline> pipeline;
  std::optional<rc::annexb::Codec> codec;
  std::mutex screenshotMutex;
  std::condition_variable_any screenshotWake;
  std::deque<std::pair<QImage, QString>> screenshotJobs;
  std::jthread screenshotWorker;
};

LiveMediaPipeline::LiveMediaPipeline(QObject* parent)
    : QObject(parent), pipeline_(std::make_unique<PipelineImpl>(*this)) {
  QSettings settings;
  constexpr int kSettingsSchema = 1;
  if (settings.value(QStringLiteral("settings/schemaVersion"), kSettingsSchema).toInt() !=
      kSettingsSchema) {
    settings.remove(QStringLiteral("transform"));
  }
  settings.setValue(QStringLiteral("settings/schemaVersion"), kSettingsSchema);
  transform_.rotationDeg =
      settings.value(QStringLiteral("transform/rotationDeg"), 0.0).toFloat();
  transform_.fit = static_cast<rc::FitMode>(std::clamp(
      settings.value(QStringLiteral("transform/fitMode"), 0).toInt(),
      static_cast<int>(rc::FitMode::Fit), static_cast<int>(rc::FitMode::Stretch)));
  transform_.flipH = settings.value(QStringLiteral("transform/flipH"), false).toBool();
  transform_.flipV = settings.value(QStringLiteral("transform/flipV"), false).toBool();
  transform_.zoom = std::clamp(
      settings.value(QStringLiteral("transform/zoom"), 1.0).toFloat(), 1.0f, 8.0f);
  recordingTimer_.setInterval(250);
  recordingTimer_.setTimerType(Qt::CoarseTimer);
  connect(&recordingTimer_, &QTimer::timeout, this,
          &LiveMediaPipeline::updateRecordingDuration);
  if (pipeline_->recorder) {
    pipeline_->recorder->setObserver(
        [this](const rcplatform::Mp4RecorderSnapshot&) {
          // Fetch at delivery time instead of capturing the callback's snapshot. A
          // synchronous stop/finalize can overtake queued Qt events; reading current
          // state prevents an old Recording event from reverting a Completed UI.
          QMetaObject::invokeMethod(
              this,
              [this] {
                if (pipeline_->recorder) {
                  applyRecorderSnapshot(pipeline_->recorder->snapshot());
                }
              },
              Qt::QueuedConnection);
        });
  } else {
    recorderState_ = rcplatform::Mp4RecorderState::Failed;
    recordingStatus_ = QStringLiteral("Recorder could not be created");
  }
  pipeline_->adaptive.sink.setObserver(
      [this](const rcplatform::FrameRingSinkSnapshot& snapshot) {
        QMetaObject::invokeMethod(
            this,
            [this, snapshot] {
              outputResolution_ = QStringLiteral("%1 x %2")
                                      .arg(snapshot.width)
                                      .arg(snapshot.height);
              switch (snapshot.state) {
                case rcplatform::FrameRingSinkState::Publishing:
                  connectionState_ = ConnectionState::ConnectedPublishing;
                  connectionLabel_ = QStringLiteral("Connected / publishing");
                  connectionDetail_ =
                      QStringLiteral("Publishing processed iPhone video as NV12.");
                  break;
                case rcplatform::FrameRingSinkState::ProducerConflict:
                  connectionState_ = ConnectionState::ProducerConflict;
                  connectionLabel_ = QStringLiteral("Producer conflict");
                  connectionDetail_ = QStringLiteral(
                      "Another RemoteCam instance owns the virtual-camera output.");
                  break;
                case rcplatform::FrameRingSinkState::Failed:
                  connectionState_ = ConnectionState::ActualFailure;
                  connectionLabel_ = QStringLiteral("Output failure");
                  connectionDetail_ = QString::fromStdWString(rcwin::hrMessage(snapshot.error));
                  break;
                case rcplatform::FrameRingSinkState::AdaptingGeometry:
                  connectionState_ = ConnectionState::WaitingForCameraConsumer;
                  connectionLabel_ = QStringLiteral("Adapting camera format");
                  connectionDetail_ =
                      QStringLiteral("Switching to the resolution requested by the consumer.");
                  break;
                case rcplatform::FrameRingSinkState::Stopped:
                case rcplatform::FrameRingSinkState::WaitingForConsumer:
                  connectionState_ = ConnectionState::WaitingForCameraConsumer;
                  connectionLabel_ = QStringLiteral("Waiting for camera consumer");
                  connectionDetail_ = QStringLiteral(
                      "Open RemoteCam in a camera application, then connect a paired iPhone.");
                  break;
              }
              emit outputChanged();
            },
            Qt::QueuedConnection);
      });
  pipeline_->adaptive.sink.setPreviewObserver(
      [this](std::shared_ptr<const rcplatform::BgraPreviewFrame> frame) {
        const uint64_t geometry =
            (static_cast<uint64_t>(frame->width) << 32u) | frame->height;
        latestPreviewGeometry_.store(geometry, std::memory_order_release);
        if (pipeline_->recorder) {
          const HRESULT recordHr = pipeline_->recorder->enqueue(frame);
          if (recordHr == E_INVALIDARG &&
              !recordingFrameErrorQueued_.exchange(true, std::memory_order_acq_rel)) {
            QMetaObject::invokeMethod(
                this, [this, recordHr] { stopRecordingForFrameError(recordHr); },
                Qt::QueuedConnection);
          }
        }
        PreviewProvider* provider = previewProvider_.load(std::memory_order_acquire);
        if (provider == nullptr) return;
        provider->update(std::move(frame));
        QMetaObject::invokeMethod(
            this,
            [this] {
              ++previewRevision_;
              emit previewChanged();
            },
            Qt::QueuedConnection);
      });
}

LiveMediaPipeline::~LiveMediaPipeline() { stop(); }

double LiveMediaPipeline::rotationDeg() const {
  std::lock_guard<std::mutex> lock(transformMutex_);
  return transform_.rotationDeg;
}

int LiveMediaPipeline::fitMode() const {
  std::lock_guard<std::mutex> lock(transformMutex_);
  return static_cast<int>(transform_.fit);
}

bool LiveMediaPipeline::flipH() const {
  std::lock_guard<std::mutex> lock(transformMutex_);
  return transform_.flipH;
}

bool LiveMediaPipeline::flipV() const {
  std::lock_guard<std::mutex> lock(transformMutex_);
  return transform_.flipV;
}

double LiveMediaPipeline::zoom() const {
  std::lock_guard<std::mutex> lock(transformMutex_);
  return transform_.zoom;
}

QString LiveMediaPipeline::previewSource() const {
  return QStringLiteral("image://live/frame?revision=%1").arg(previewRevision_);
}

bool LiveMediaPipeline::recording() const {
  return recorderState_ == rcplatform::Mp4RecorderState::Starting ||
         recorderState_ == rcplatform::Mp4RecorderState::Recording;
}

bool LiveMediaPipeline::recordingCanToggle() const {
  return pipeline_ && pipeline_->recorder &&
         recorderState_ != rcplatform::Mp4RecorderState::Finalizing;
}

QString LiveMediaPipeline::recordingDuration() const {
  const qint64 hours = recordingDurationSeconds_ / 3600;
  const qint64 minutes = (recordingDurationSeconds_ / 60) % 60;
  const qint64 seconds = recordingDurationSeconds_ % 60;
  if (hours > 0) {
    return QStringLiteral("%1:%2:%3")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
  }
  return QStringLiteral("%1:%2")
      .arg(minutes, 2, 10, QLatin1Char('0'))
      .arg(seconds, 2, 10, QLatin1Char('0'));
}

void LiveMediaPipeline::start() {
  if (worker_.joinable()) return;
  pipeline_->adaptive.sink.start();
  pipeline_->startScreenshots(*this);
  worker_ = std::jthread([this](std::stop_token token) { run(token); });
}

void LiveMediaPipeline::stop() {
  if (worker_.joinable()) {
    worker_.request_stop();
    queueWake_.notify_all();
    worker_.join();
  }
  pipeline_->adaptive.sink.stop();
  // Stop the source of preview frames before draining the recorder queue. Once stop()
  // returns, a successful recorder stop guarantees the final MP4 has been closed and
  // atomically published; no late sink callback can enqueue behind that boundary.
  stopRecording();
  pipeline_->stopScreenshots();
  pipeline_->pipeline.reset();
  pipeline_->decoder.reset();
  pipeline_->codec.reset();
}

void LiveMediaPipeline::setProducerConflict() {
  connectionState_ = ConnectionState::ProducerConflict;
  connectionLabel_ = QStringLiteral("Producer conflict");
  connectionDetail_ = QStringLiteral(
      "Another RemoteCam instance is already publishing in this Windows session.");
  emit outputChanged();
}

void LiveMediaPipeline::setStartupFailure(const QString& detail) {
  connectionState_ = ConnectionState::ActualFailure;
  connectionLabel_ = QStringLiteral("Startup failure");
  connectionDetail_ = detail;
  emit outputChanged();
}

void LiveMediaPipeline::setKeyframeRequester(std::function<void()> requester) {
  std::lock_guard<std::mutex> lock(queueMutex_);
  keyframeRequester_ = std::move(requester);
}

void LiveMediaPipeline::setPreviewProvider(PreviewProvider* provider) {
  previewProvider_.store(provider, std::memory_order_release);
}

bool LiveMediaPipeline::consume(const rcbackend::EncodedAccessUnit& unit) {
  bool requestKeyframe = false;
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (waitingForKeyframe_ && !unit.keyframe) return true;
    if (queue_.size() >= kMaxQueuedUnits ||
        queueBytes_ + unit.bytes.size() > kMaxQueuedBytes) {
      queue_.clear();
      queueBytes_ = 0;
      waitingForKeyframe_ = true;
      requestKeyframe = true;
    }
    if (!requestKeyframe) {
      if (unit.keyframe) waitingForKeyframe_ = false;
      queue_.push_back(unit);
      queueBytes_ += unit.bytes.size();
    }
  }
  if (requestKeyframe) requestRecoveryKeyframe();
  queueWake_.notify_one();
  return true;
}

void LiveMediaPipeline::reset(const rc::control::StreamConfig& config, uint64_t generation) {
  if (config.fps != 0) streamFrameRate_.store(config.fps, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    queue_.clear();
    queueBytes_ = 0;
    waitingForKeyframe_ = true;
    pendingReset_ = std::make_pair(config, generation);
  }
  queueWake_.notify_one();
}

void LiveMediaPipeline::run(std::stop_token stopToken) {
  while (!stopToken.stop_requested()) {
    rcbackend::EncodedAccessUnit unit;
    {
      std::unique_lock<std::mutex> lock(queueMutex_);
      queueWake_.wait(lock, stopToken,
                      [this] { return pendingReset_.has_value() || !queue_.empty(); });
      if (stopToken.stop_requested()) break;
      if (pendingReset_.has_value()) {
        pendingReset_.reset();
        pipeline_->pipeline.reset();
        pipeline_->decoder.reset();
        pipeline_->codec.reset();
        continue;
      }
      unit = std::move(queue_.front());
      queueBytes_ -= unit.bytes.size();
      queue_.pop_front();
    }

    const rc::annexb::Codec codec = platformCodec(unit.codec);
    const HRESULT decoderHr = pipeline_->selectCodec(codec);
    if (FAILED(decoderHr)) {
      postFailure(decoderError(decoderHr));
      std::lock_guard<std::mutex> lock(queueMutex_);
      queue_.clear();
      queueBytes_ = 0;
      waitingForKeyframe_ = true;
      continue;
    }

    rcplatform::EncodedAccessUnit platformUnit;
    platformUnit.bytes = unit.bytes.data();
    platformUnit.size = unit.bytes.size();
    platformUnit.codec = codec;
    platformUnit.keyframe = unit.keyframe;
    platformUnit.ptsMicros = unit.ptsMicros;
    const rcplatform::PipelineOutcome outcome =
        pipeline_->pipeline->push(platformUnit, rc::TransformParams{});
    if (outcome.result == rcplatform::PipelineResult::Published ||
        outcome.result == rcplatform::PipelineResult::DecoderNeedsMoreInput) {
      continue;
    }

    pipeline_->decoder->flush();
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      queue_.clear();
      queueBytes_ = 0;
      waitingForKeyframe_ = true;
    }
    postFailure(QStringLiteral("Video pipeline recovery: %1")
                    .arg(QString::fromStdWString(rcwin::hrMessage(outcome.detail))));
    requestRecoveryKeyframe();
  }
}

void LiveMediaPipeline::postFailure(QString detail) {
  QMetaObject::invokeMethod(
      this,
      [this, detail = std::move(detail)] {
        connectionState_ = ConnectionState::ActualFailure;
        connectionLabel_ = QStringLiteral("Video pipeline failure");
        connectionDetail_ = detail;
        emit outputChanged();
      },
      Qt::QueuedConnection);
}

void LiveMediaPipeline::requestRecoveryKeyframe() {
  std::function<void()> callback;
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    callback = keyframeRequester_;
  }
  if (callback) callback();
}

rc::TransformParams LiveMediaPipeline::transformFor(uint32_t sourceWidth,
                                                     uint32_t sourceHeight,
                                                     uint32_t outputWidth,
                                                     uint32_t outputHeight) const {
  std::lock_guard<std::mutex> lock(transformMutex_);
  rc::TransformParams params = transform_;
  params.srcWidth = static_cast<int>(sourceWidth);
  params.srcHeight = static_cast<int>(sourceHeight);
  params.dstWidth = static_cast<int>(outputWidth);
  params.dstHeight = static_cast<int>(outputHeight);
  if (params.fit == rc::FitMode::Fill) rc::clampPanForCoverage(params);
  return params;
}

void LiveMediaPipeline::persistTransform() const {
  std::lock_guard<std::mutex> lock(transformMutex_);
  QSettings settings;
  settings.setValue(QStringLiteral("settings/schemaVersion"), 1);
  settings.setValue(QStringLiteral("transform/rotationDeg"), transform_.rotationDeg);
  settings.setValue(QStringLiteral("transform/fitMode"), static_cast<int>(transform_.fit));
  settings.setValue(QStringLiteral("transform/flipH"), transform_.flipH);
  settings.setValue(QStringLiteral("transform/flipV"), transform_.flipV);
  settings.setValue(QStringLiteral("transform/zoom"), transform_.zoom);
}

void LiveMediaPipeline::setRotationDeg(double value) {
  if (!std::isfinite(value)) return;
  {
    std::lock_guard<std::mutex> lock(transformMutex_);
    transform_.rotationDeg = static_cast<float>(std::remainder(value, 360.0));
  }
  persistTransform();
  emit transformChanged();
}

void LiveMediaPipeline::setFitMode(int value) {
  if (value < static_cast<int>(rc::FitMode::Fit) ||
      value > static_cast<int>(rc::FitMode::Stretch)) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(transformMutex_);
    transform_.fit = static_cast<rc::FitMode>(value);
  }
  persistTransform();
  emit transformChanged();
}

void LiveMediaPipeline::setFlipH(bool value) {
  {
    std::lock_guard<std::mutex> lock(transformMutex_);
    transform_.flipH = value;
  }
  persistTransform();
  emit transformChanged();
}

void LiveMediaPipeline::setFlipV(bool value) {
  {
    std::lock_guard<std::mutex> lock(transformMutex_);
    transform_.flipV = value;
  }
  persistTransform();
  emit transformChanged();
}

void LiveMediaPipeline::setZoom(double value) {
  if (!std::isfinite(value) || value < 1.0 || value > 8.0) return;
  {
    std::lock_guard<std::mutex> lock(transformMutex_);
    transform_.zoom = static_cast<float>(value);
  }
  persistTransform();
  emit transformChanged();
}

void LiveMediaPipeline::resetTransform() {
  {
    std::lock_guard<std::mutex> lock(transformMutex_);
    transform_ = rc::TransformParams{};
    transform_.fit = rc::FitMode::Fit;
    transform_.zoom = 1.0f;
  }
  persistTransform();
  emit transformChanged();
}

void LiveMediaPipeline::takeScreenshot() {
  PreviewProvider* provider = previewProvider_.load(std::memory_order_acquire);
  const QImage image = provider == nullptr ? QImage{} : provider->snapshot();
  if (image.isNull()) {
    screenshotStatus_ = QStringLiteral("No processed frame is available yet");
    emit screenshotChanged();
    return;
  }

  const QString pictures = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
  const QString directory = QDir(pictures).filePath(QStringLiteral("RemoteCam"));
  const QString filename =
      QStringLiteral("RemoteCam-%1.png")
          .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")));
  const QString path = QDir(directory).filePath(filename);
  {
    std::lock_guard<std::mutex> lock(pipeline_->screenshotMutex);
    if (pipeline_->screenshotJobs.size() >= 2) pipeline_->screenshotJobs.pop_front();
    pipeline_->screenshotJobs.emplace_back(image, path);
  }
  screenshotStatus_ = QStringLiteral("Saving screenshot…");
  emit screenshotChanged();
  pipeline_->screenshotWake.notify_one();
}

void LiveMediaPipeline::startRecording() {
  if (!pipeline_->recorder || recording() ||
      recorderState_ == rcplatform::Mp4RecorderState::Finalizing) {
    return;
  }

  const uint64_t geometry = latestPreviewGeometry_.load(std::memory_order_acquire);
  const uint32_t width = static_cast<uint32_t>(geometry >> 32u);
  const uint32_t height = static_cast<uint32_t>(geometry);
  if (width == 0 || height == 0) {
    recorderState_ = rcplatform::Mp4RecorderState::Failed;
    recordingStatus_ = QStringLiteral("No processed frame is available to record yet");
    emit recordingChanged();
    return;
  }

  const QString videos =
      QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
  const QString directory = QDir(videos).filePath(QStringLiteral("RemoteCam"));
  if (videos.isEmpty() || !QDir().mkpath(directory)) {
    recorderState_ = rcplatform::Mp4RecorderState::Failed;
    recordingStatus_ = QStringLiteral("Could not create the Videos\\RemoteCam folder");
    emit recordingChanged();
    return;
  }
  const QString filename =
      QStringLiteral("RemoteCam-%1.mp4")
          .arg(QDateTime::currentDateTime().toString(
              QStringLiteral("yyyyMMdd-HHmmss-zzz")));
  const QString path = QDir(directory).filePath(filename);

  const uint32_t fps =
      (std::max)(1u, streamFrameRate_.load(std::memory_order_acquire));
  const uint64_t estimatedBitrate =
      static_cast<uint64_t>(width) * height * fps / 8u;
  rcplatform::Mp4RecorderConfig config;
  config.outputPath = path.toStdWString();
  config.width = width;
  config.height = height;
  config.fpsNumerator = fps;
  config.fpsDenominator = 1;
  config.bitrateBitsPerSecond = static_cast<uint32_t>(
      std::clamp<uint64_t>(estimatedBitrate, 4'000'000u, 50'000'000u));
  config.queueCapacity = 4;

  recordingFrameErrorQueued_.store(false, std::memory_order_release);
  recordingDurationSeconds_ = 0;
  recordingPath_ = path;
  const HRESULT hr = pipeline_->recorder->start(config);
  if (FAILED(hr)) {
    recorderState_ = rcplatform::Mp4RecorderState::Failed;
    recordingStatus_ = QStringLiteral("Recording could not start: %1")
                           .arg(QString::fromStdWString(rcwin::hrMessage(hr)));
    emit recordingChanged();
    return;
  }
  applyRecorderSnapshot(pipeline_->recorder->snapshot());
}

void LiveMediaPipeline::stopRecording() {
  if (!pipeline_ || !pipeline_->recorder) return;
  pipeline_->recorder->stop();
  applyRecorderSnapshot(pipeline_->recorder->snapshot());
}

void LiveMediaPipeline::toggleRecording() {
  if (!recordingCanToggle()) return;
  if (recording()) stopRecording();
  else startRecording();
}

void LiveMediaPipeline::applyRecorderSnapshot(
    const rcplatform::Mp4RecorderSnapshot& snapshot) {
  const rcplatform::Mp4RecorderState previous = recorderState_;
  recorderState_ = snapshot.state;
  recordingDroppedFrames_ = snapshot.droppedFrames;
  if (!snapshot.outputPath.empty()) {
    recordingPath_ = QString::fromStdWString(snapshot.outputPath);
  }
  if (snapshot.state == rcplatform::Mp4RecorderState::Failed &&
      !snapshot.partialPath.empty()) {
    recordingPath_ = QString::fromStdWString(snapshot.partialPath);
  }

  if (snapshot.state == rcplatform::Mp4RecorderState::Recording &&
      previous != rcplatform::Mp4RecorderState::Recording) {
    recordingDurationSeconds_ = 0;
    recordingElapsed_.restart();
    recordingTimer_.start();
  } else if (snapshot.state != rcplatform::Mp4RecorderState::Recording &&
             previous == rcplatform::Mp4RecorderState::Recording) {
    if (recordingElapsed_.isValid()) {
      recordingDurationSeconds_ = recordingElapsed_.elapsed() / 1000;
    }
    recordingTimer_.stop();
  }
  if (snapshot.state != rcplatform::Mp4RecorderState::Recording &&
      snapshot.duration100ns != 0) {
    recordingDurationSeconds_ =
        static_cast<qint64>(snapshot.duration100ns / 10000000u);
  }

  switch (snapshot.state) {
    case rcplatform::Mp4RecorderState::Idle:
      recordingStatus_ = QStringLiteral("Ready to record");
      break;
    case rcplatform::Mp4RecorderState::Starting:
      recordingStatus_ = QStringLiteral("Starting H.264 MP4 recorder...");
      break;
    case rcplatform::Mp4RecorderState::Recording:
      recordingStatus_ =
          snapshot.encoderPath == rcplatform::Mp4EncoderPath::SoftwareOnly
              ? QStringLiteral("Recording MP4 with software encoder fallback")
              : QStringLiteral("Recording H.264 MP4");
      break;
    case rcplatform::Mp4RecorderState::Finalizing:
      recordingStatus_ = QStringLiteral("Finalizing MP4...");
      break;
    case rcplatform::Mp4RecorderState::Completed:
      recordingStatus_ = QStringLiteral("Recording saved");
      recordingFrameErrorQueued_.store(false, std::memory_order_release);
      break;
    case rcplatform::Mp4RecorderState::Failed:
      if (!snapshot.partialPath.empty()) {
        recordingStatus_ = QStringLiteral("Could not publish the final name; saved partial MP4");
      } else if (FAILED(snapshot.error)) {
        recordingStatus_ = QStringLiteral("Recording failed: %1")
                               .arg(QString::fromStdWString(
                                   rcwin::hrMessage(snapshot.error)));
      }
      recordingFrameErrorQueued_.store(false, std::memory_order_release);
      break;
  }
  emit recordingChanged();
}

void LiveMediaPipeline::stopRecordingForFrameError(HRESULT error) {
  if (!recording()) {
    recordingFrameErrorQueued_.store(false, std::memory_order_release);
    return;
  }
  stopRecording();
  if (recorderState_ == rcplatform::Mp4RecorderState::Completed) {
    recordingStatus_ =
        error == E_INVALIDARG
            ? QStringLiteral("Recording saved early because the output size changed")
            : QStringLiteral("Recording saved early after a frame error");
    emit recordingChanged();
  }
  recordingFrameErrorQueued_.store(false, std::memory_order_release);
}

void LiveMediaPipeline::updateRecordingDuration() {
  if (recorderState_ != rcplatform::Mp4RecorderState::Recording ||
      !recordingElapsed_.isValid()) {
    return;
  }
  const qint64 seconds = recordingElapsed_.elapsed() / 1000;
  if (seconds == recordingDurationSeconds_) return;
  recordingDurationSeconds_ = seconds;
  emit recordingChanged();
}

}  // namespace rcapp
