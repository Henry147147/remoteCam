#include "frame_producer.h"

#include <QMetaObject>
#include <cstdint>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>

#include "rcwin/hr.h"
#include "rcwin/nv12.h"
#include "rcwin/shm_ring.h"
#include "rcwin/test_pattern.h"

namespace rcapp {
namespace {

constexpr int kOutputWidth = 1920;
constexpr int kOutputHeight = 1080;
constexpr int kOutputFps = 30;
constexpr DWORD kConsumerPollMillis = 250;
constexpr HRESULT kRingNotFound = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
constexpr HRESULT kRingBusy = HRESULT_FROM_WIN32(ERROR_TIMEOUT);

// M1 advertises exactly this geometry. Keep these assertions beside the
// producer so a later UI edit cannot silently publish a format that FrameSource
// will reject.
static_assert(kOutputWidth % 2 == 0);
static_assert(kOutputHeight % 2 == 0);
static_assert(kOutputWidth <= static_cast<int>(rcwin::kRingMaxWidth));
static_assert(kOutputHeight <= static_cast<int>(rcwin::kRingMaxHeight));
static_assert(static_cast<uint64_t>(kOutputWidth) * kOutputHeight * 3u / 2u <=
              rcwin::kRingSlotBytes);

QString hresultDetail(const wchar_t* operation, HRESULT hr) {
  return QStringLiteral("%1 failed: %2")
      .arg(QString::fromWCharArray(operation), QString::fromStdWString(rcwin::hrMessage(hr)));
}

bool waitOrStop(HANDLE stopEvent, DWORD milliseconds) {
  return ::WaitForSingleObject(stopEvent, milliseconds) == WAIT_OBJECT_0;
}

LONGLONG qpcTo100ns(LONGLONG ticks, LONGLONG frequency) {
  const LONGLONG wholeSeconds = ticks / frequency;
  const LONGLONG remainder = ticks % frequency;
  return wholeSeconds * 10000000LL + remainder * 10000000LL / frequency;
}

}  // namespace

FrameProducer::FrameProducer(QObject* parent) : QObject(parent) {}

FrameProducer::~FrameProducer() {
  stop();
  if (stopEvent_) {
    ::CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
  }
}

QString FrameProducer::connectionLabel() const {
  switch (connectionState_) {
    case ConnectionState::WaitingForCameraConsumer:
      return QStringLiteral("Waiting for camera consumer");
    case ConnectionState::ConnectedPublishing:
      return QStringLiteral("Connected / publishing");
    case ConnectionState::ProducerConflict:
      return QStringLiteral("Producer conflict");
    case ConnectionState::ActualFailure:
      return QStringLiteral("Actual failure");
  }
  return QStringLiteral("Actual failure");
}

void FrameProducer::start() {
  if (worker_.joinable() || connectionState_ == ConnectionState::ProducerConflict) return;

  if (stopEvent_) {
    ::CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
  }
  stopEvent_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!stopEvent_) {
    setStartupFailure(hresultDetail(L"CreateEventW", rcwin::hrFromLastError()));
    return;
  }

  try {
    worker_ = std::jthread([this](std::stop_token stopToken) { run(stopToken); });
  } catch (const std::system_error& error) {
    ::CloseHandle(stopEvent_);
    stopEvent_ = nullptr;
    setStartupFailure(QStringLiteral("Could not start the producer worker: %1")
                          .arg(QString::fromLocal8Bit(error.what())));
  }
}

void FrameProducer::stop() {
  if (!worker_.joinable()) return;
  worker_.request_stop();
  if (stopEvent_) ::SetEvent(stopEvent_);
  worker_.join();
  ::CloseHandle(stopEvent_);
  stopEvent_ = nullptr;
}

void FrameProducer::setProducerConflict() {
  connectionState_ = ConnectionState::ProducerConflict;
  connectionDetail_ = QStringLiteral(
      "Another RemoteCam producer is already running in this Windows session. "
      "Close that window before starting a second producer.");
  emit connectionStateChanged();
}

void FrameProducer::setStartupFailure(const QString& detail) {
  connectionState_ = ConnectionState::ActualFailure;
  connectionDetail_ = detail;
  emit connectionStateChanged();
}

void FrameProducer::postState(ConnectionState state, QString detail) {
  QMetaObject::invokeMethod(
      this,
      [this, state, detail = std::move(detail)]() mutable {
        if (connectionState_ == state && connectionDetail_ == detail) return;
        connectionState_ = state;
        connectionDetail_ = std::move(detail);
        emit connectionStateChanged();
      },
      Qt::QueuedConnection);
}

void FrameProducer::run(std::stop_token stopToken) {
  const rcwin::Nv12Layout layout = rcwin::nv12Layout(kOutputWidth, kOutputHeight);
  const uint64_t minimumBytes = static_cast<uint64_t>(layout.stride) * layout.height * 3u / 2u;
  if (layout.width != kOutputWidth || layout.height != kOutputHeight || layout.width % 2 != 0 ||
      layout.height % 2 != 0 || layout.stride < layout.width || layout.totalSize < minimumBytes ||
      layout.totalSize > rcwin::kRingSlotBytes ||
      layout.totalSize > std::numeric_limits<uint32_t>::max()) {
    RC_ERR(L"fixed M1 output geometry is invalid");
    postState(ConnectionState::ActualFailure,
              QStringLiteral("The fixed NV12 output geometry is invalid."));
    return;
  }

  HANDLE timer = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
  if (!timer) timer = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  if (!timer) {
    const HRESULT hr = rcwin::hrFromLastError();
    RC_ERR(L"CreateWaitableTimerExW failed: %s", rcwin::hrMessage(hr).c_str());
    postState(ConnectionState::ActualFailure, hresultDetail(L"CreateWaitableTimerExW", hr));
    return;
  }

  LARGE_INTEGER qpcFrequency{};
  if (!::QueryPerformanceFrequency(&qpcFrequency) || qpcFrequency.QuadPart <= 0) {
    const HRESULT hr = rcwin::hrFromLastError();
    RC_ERR(L"QueryPerformanceFrequency failed: %s", rcwin::hrMessage(hr).c_str());
    postState(ConnectionState::ActualFailure, hresultDetail(L"QueryPerformanceFrequency", hr));
    ::CloseHandle(timer);
    return;
  }

  std::vector<uint8_t> frame(layout.totalSize);
  rcwin::FrameRing ring;
  ConnectionState workerState = ConnectionState::ActualFailure;
  auto transition = [&](ConnectionState next, const QString& detail, const wchar_t* log) {
    if (workerState == next) return;
    workerState = next;
    RC_LOG(L"%s", log);
    postState(next, detail);
  };

  transition(ConnectionState::WaitingForCameraConsumer,
             QStringLiteral("Select the RemoteCam virtual camera in a camera application."),
             L"waiting for camera consumer");

  LARGE_INTEGER pacingOrigin{};
  const LONGLONG interval100ns = 10000000LL / kOutputFps;
  uint64_t pacingTick = 0;
  uint64_t frameIndex = 0;

  while (!stopToken.stop_requested()) {
    if (!ring.valid()) {
      const HRESULT openHr = ring.open(true);
      if (FAILED(openHr)) {
        if (openHr == kRingNotFound) {
          transition(ConnectionState::WaitingForCameraConsumer,
                     QStringLiteral("Select the RemoteCam virtual camera in a "
                                    "camera application."),
                     L"camera consumer disconnected; waiting");
          if (waitOrStop(stopEvent_, kConsumerPollMillis)) break;
          continue;
        }

        const QString detail = hresultDetail(L"FrameRing::open", openHr);
        RC_ERR(L"FrameRing::open failed: %s", rcwin::hrMessage(openHr).c_str());
        workerState = ConnectionState::ActualFailure;
        postState(ConnectionState::ActualFailure, detail);
        ring.close();
        break;
      }

      // A consumer can be absent for minutes. Re-anchor every successful open
      // so none of those missed 30 Hz ticks is replayed as a catch-up burst.
      if (!::QueryPerformanceCounter(&pacingOrigin)) {
        const HRESULT hr = rcwin::hrFromLastError();
        RC_ERR(L"QueryPerformanceCounter failed: %s", rcwin::hrMessage(hr).c_str());
        postState(ConnectionState::ActualFailure,
                  hresultDetail(L"QueryPerformanceCounter", hr));
        break;
      }
      pacingTick = 0;
      transition(ConnectionState::ConnectedPublishing,
                 QStringLiteral("Publishing fixed NV12 1920 x 1080 video at 30 fps."),
                 L"camera consumer connected; publishing");
    }

    rcwin::renderPattern(frame.data(), layout, frameIndex, rcwin::PatternStyle::Writer);

    rcwin::FrameInfo info;
    info.width = static_cast<uint32_t>(layout.width);
    info.height = static_cast<uint32_t>(layout.height);
    info.stride = static_cast<uint32_t>(layout.stride);
    info.format = rcwin::kFourccNv12;
    info.ptsMicros = ::GetTickCount64() * 1000ull;
    info.bytesUsed = static_cast<uint32_t>(frame.size());

    const HRESULT writeHr =
        ring.writeFrame(frame.data(), static_cast<uint32_t>(frame.size()), info);
    if (FAILED(writeHr)) {
      if (writeHr == kRingBusy) {
        // A consumer generation transition or an unsupported second writer held the
        // zero-timeout guard. Dropping one frame preserves the non-blocking contract.
      } else if (writeHr == kRingNotFound) {
        // The consumer owns the ring lifetime. Discard the stale mapping and return to
        // quiet polling without an alarming notification.
        RC_LOG(L"camera consumer closed; waiting for it to reopen");
        ring.close();
        workerState = ConnectionState::WaitingForCameraConsumer;
        postState(ConnectionState::WaitingForCameraConsumer,
                  QStringLiteral("The camera consumer closed. Waiting for it to reopen."));
        if (waitOrStop(stopEvent_, kConsumerPollMillis)) break;
        continue;
      } else {
        const QString detail = hresultDetail(L"FrameRing::writeFrame", writeHr);
        RC_ERR(L"FrameRing::writeFrame failed: %s", rcwin::hrMessage(writeHr).c_str());
        workerState = ConnectionState::ActualFailure;
        postState(ConnectionState::ActualFailure, detail);
        ring.close();
        break;
      }
    }

    if (SUCCEEDED(writeHr)) ++frameIndex;
    ++pacingTick;

    // Every deadline is derived from the reconnect origin. Timer/wake latency
    // can make one frame late, but it cannot accumulate into a permanently
    // slower frame rate.
    LARGE_INTEGER now{};
    if (!::QueryPerformanceCounter(&now)) {
      const HRESULT hr = rcwin::hrFromLastError();
      RC_ERR(L"QueryPerformanceCounter failed: %s", rcwin::hrMessage(hr).c_str());
      postState(ConnectionState::ActualFailure,
                hresultDetail(L"QueryPerformanceCounter", hr));
      break;
    }
    const LONGLONG elapsed100ns =
        qpcTo100ns(now.QuadPart - pacingOrigin.QuadPart, qpcFrequency.QuadPart);
    LONGLONG untilDeadline = static_cast<LONGLONG>(pacingTick) * interval100ns - elapsed100ns;
    if (untilDeadline < 0) untilDeadline = 0;

    LARGE_INTEGER due{};
    due.QuadPart = -untilDeadline;
    if (!::SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      const HRESULT hr = rcwin::hrFromLastError();
      RC_ERR(L"SetWaitableTimer failed: %s", rcwin::hrMessage(hr).c_str());
      postState(ConnectionState::ActualFailure, hresultDetail(L"SetWaitableTimer", hr));
      break;
    }

    const HANDLE waits[] = {timer, stopEvent_};
    const DWORD waitResult = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (waitResult == WAIT_OBJECT_0 + 1) break;
    if (waitResult != WAIT_OBJECT_0) {
      const HRESULT hr = rcwin::hrFromLastError();
      RC_ERR(L"WaitForMultipleObjects failed: %s", rcwin::hrMessage(hr).c_str());
      postState(ConnectionState::ActualFailure, hresultDetail(L"WaitForMultipleObjects", hr));
      break;
    }
  }

  ring.close();
  ::CancelWaitableTimer(timer);
  ::CloseHandle(timer);
}

}  // namespace rcapp
