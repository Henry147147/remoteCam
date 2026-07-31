#include "rcplatform/mp4_recorder.h"

#include <mferror.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

#include "mp4_recorder_internal.h"
#include "rcplatform/pixel_convert.h"
#include "rcwin/hr.h"
#include "rcwin/nv12.h"
#include "rcwin/shm_ring.h"

namespace rcplatform {
namespace {

constexpr size_t kMaximumQueueCapacity = 16;
std::atomic<uint64_t> g_partialSequence{0};

bool hasMp4Extension(const std::wstring& path) {
  if (path.size() < 4) return false;
  return ::CompareStringOrdinal(path.c_str() + path.size() - 4, 4, L".mp4", 4,
                                TRUE) == CSTR_EQUAL;
}

HRESULT absolutePath(const std::wstring& input, std::wstring& output) {
  if (input.empty()) return E_INVALIDARG;
  const DWORD required = ::GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
  if (required == 0) return rcwin::hrFromLastError();
  std::vector<wchar_t> buffer(static_cast<size_t>(required) + 1u);
  const DWORD length = ::GetFullPathNameW(input.c_str(),
                                          static_cast<DWORD>(buffer.size()),
                                          buffer.data(), nullptr);
  if (length == 0) return rcwin::hrFromLastError();
  if (length >= buffer.size()) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
  output.assign(buffer.data(), length);
  return S_OK;
}

std::wstring makePartialPath(const std::wstring& outputPath) {
  const uint64_t sequence = g_partialSequence.fetch_add(1, std::memory_order_relaxed) + 1;
  std::wstring partial = outputPath.substr(0, outputPath.size() - 4);
  partial += L".partial.";
  partial += std::to_wstring(::GetCurrentProcessId());
  partial += L".";
  partial += std::to_wstring(sequence);
  partial += L".mp4";
  return partial;
}

HRESULT validateConfig(const Mp4RecorderConfig& config) {
  if (config.outputPath.empty() || !hasMp4Extension(config.outputPath) ||
      config.width == 0 || config.height == 0 || (config.width & 1u) != 0 ||
      (config.height & 1u) != 0 || config.width > rcwin::kRingMaxWidth ||
      config.height > rcwin::kRingMaxHeight || config.fpsNumerator == 0 ||
      config.fpsDenominator == 0 || config.bitrateBitsPerSecond == 0 ||
      config.queueCapacity == 0 || config.queueCapacity > kMaximumQueueCapacity) {
    return E_INVALIDARG;
  }
  const uint64_t frameDuration =
      10000000ull * config.fpsDenominator / config.fpsNumerator;
  const uint64_t bgraBytes = static_cast<uint64_t>(config.width) * config.height * 4u;
  const uint64_t nv12Bytes = static_cast<uint64_t>(config.width) * config.height * 3u / 2u;
  if (frameDuration == 0 || bgraBytes > (std::numeric_limits<size_t>::max)() ||
      nv12Bytes > (std::numeric_limits<DWORD>::max)()) {
    return E_INVALIDARG;
  }
  return S_OK;
}

bool validFrame(const BgraPreviewFrame& frame, const Mp4RecorderConfig& config) {
  if (frame.width != config.width || frame.height != config.height ||
      frame.stride < frame.width * 4u) {
    return false;
  }
  const uint64_t required = static_cast<uint64_t>(frame.stride) * frame.height;
  return required <= frame.pixels.size();
}

class MonotonicTimeline {
 public:
  explicit MonotonicTimeline(const Mp4RecorderConfig& config)
      : duration_(static_cast<LONGLONG>(
            10000000ull * config.fpsDenominator / config.fpsNumerator)) {}

  LONGLONG next(uint64_t ptsMicros) {
    if (!haveOrigin_) {
      haveOrigin_ = true;
      originMicros_ = ptsMicros;
      last_ = 0;
      return 0;
    }

    LONGLONG candidate = 0;
    if (ptsMicros >= originMicros_) {
      const uint64_t delta = ptsMicros - originMicros_;
      const uint64_t maximum =
          static_cast<uint64_t>((std::numeric_limits<LONGLONG>::max)());
      candidate = delta > maximum / 10u
                      ? (std::numeric_limits<LONGLONG>::max)()
                      : static_cast<LONGLONG>(delta * 10u);
    }

    const LONGLONG maximum = (std::numeric_limits<LONGLONG>::max)();
    const LONGLONG minimumNext = last_ > maximum - duration_
                                    ? maximum
                                    : last_ + duration_;
    if (candidate < minimumNext) candidate = minimumNext;
    last_ = candidate;
    return candidate;
  }

  LONGLONG duration() const { return duration_; }

 private:
  bool haveOrigin_ = false;
  uint64_t originMicros_ = 0;
  LONGLONG last_ = -1;
  LONGLONG duration_ = 0;
};

class Mp4Recorder final : public IMp4Recorder {
 public:
  explicit Mp4Recorder(detail::Mp4WriterFactory factory)
      : writerFactory_(std::move(factory)) {}

  ~Mp4Recorder() override { stop(); }

  HRESULT start(const Mp4RecorderConfig& requested) override {
    RC_RETURN_IF_FAILED(validateConfig(requested));

    Mp4RecorderConfig normalized = requested;
    RC_RETURN_IF_FAILED(absolutePath(requested.outputPath, normalized.outputPath));
    const std::wstring partialPath = makePartialPath(normalized.outputPath);

    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (sessionActive_) return MF_E_INVALIDREQUEST;
    }
    if (worker_.joinable()) worker_.join();

    // A unique name should not exist, but remove a stale file if a pid/sequence pair
    // was reused after a crash and reboot. The final output is never touched here.
    if (!::DeleteFileW(partialPath.c_str()) &&
        ::GetLastError() != ERROR_FILE_NOT_FOUND) {
      return rcwin::hrFromLastError();
    }

    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      config_ = normalized;
      partialPath_ = partialPath;
      queue_.clear();
      stopRequested_ = false;
      sessionActive_ = true;
    }
    replaceSnapshot([&](Mp4RecorderSnapshot& current) {
      current = Mp4RecorderSnapshot{};
      current.state = Mp4RecorderState::Starting;
      current.outputPath = normalized.outputPath;
      current.partialPath = partialPath;
    }, true);

    try {
      worker_ = std::thread(&Mp4Recorder::run, this);
    } catch (const std::system_error&) {
      {
        std::lock_guard<std::mutex> lock(queueMutex_);
        sessionActive_ = false;
      }
      const HRESULT hr = HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
      replaceSnapshot([&](Mp4RecorderSnapshot& current) {
        current.state = Mp4RecorderState::Failed;
        current.error = hr;
        current.partialPath.clear();
      }, true);
      return hr;
    }
    return S_OK;
  }

  HRESULT enqueue(std::shared_ptr<const BgraPreviewFrame> frame) override {
    if (!frame) return E_POINTER;

    bool dropped = false;
    size_t queued = 0;
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (!sessionActive_ || stopRequested_) return MF_E_NOTACCEPTING;
      if (!validFrame(*frame, config_)) return E_INVALIDARG;
      if (queue_.size() >= config_.queueCapacity) {
        queue_.pop_front();
        dropped = true;
      }
      queue_.push_back(std::move(frame));
      queued = queue_.size();
      replaceSnapshot([&](Mp4RecorderSnapshot& current) {
        ++current.submittedFrames;
        if (dropped) ++current.droppedFrames;
        current.queuedFrames = queued;
      }, false);
    }
    queueWake_.notify_one();
    if (dropped) notifyCurrent();
    return dropped ? S_FALSE : S_OK;
  }

  HRESULT stop() override {
    std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      if (sessionActive_) {
        stopRequested_ = true;
        sessionActive_ = false;
      }
    }
    queueWake_.notify_one();
    if (worker_.joinable()) worker_.join();
    return snapshot().error;
  }

  Mp4RecorderSnapshot snapshot() const override {
    std::lock_guard<std::mutex> lock(statusMutex_);
    return status_;
  }

  void setObserver(Observer observer) override {
    Mp4RecorderSnapshot current;
    Observer callback;
    {
      std::lock_guard<std::mutex> lock(statusMutex_);
      observer_ = std::move(observer);
      callback = observer_;
      current = status_;
    }
    if (callback) callback(current);
  }

 private:
  template <typename Mutation>
  void replaceSnapshot(Mutation&& mutation, bool notify) {
    Observer callback;
    Mp4RecorderSnapshot current;
    {
      std::lock_guard<std::mutex> lock(statusMutex_);
      mutation(status_);
      if (!notify || !observer_) return;
      callback = observer_;
      current = status_;
    }
    callback(current);
  }

  void notifyCurrent() {
    Observer callback;
    Mp4RecorderSnapshot current;
    {
      std::lock_guard<std::mutex> lock(statusMutex_);
      callback = observer_;
      current = status_;
    }
    if (callback) callback(current);
  }

  void discardAndFail(HRESULT error, size_t currentFrames,
                      const std::wstring& partialPath) {
    size_t discarded = currentFrames;
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      discarded += queue_.size();
      queue_.clear();
      stopRequested_ = true;
      sessionActive_ = false;
    }
    ::DeleteFileW(partialPath.c_str());
    replaceSnapshot([&](Mp4RecorderSnapshot& current) {
      current.state = Mp4RecorderState::Failed;
      current.error = error;
      current.droppedFrames += discarded;
      current.queuedFrames = 0;
      current.partialPath.clear();
    }, true);
  }

  std::unique_ptr<detail::IMp4Writer> openWriter(
      const Mp4RecorderConfig& config, const std::wstring& partialPath,
      Mp4EncoderPath& path, HRESULT& result) {
    std::unique_ptr<detail::IMp4Writer> writer = writerFactory_();
    if (!writer) {
      result = E_OUTOFMEMORY;
      return nullptr;
    }
    result = writer->open(config, partialPath, true);
    if (SUCCEEDED(result)) {
      path = Mp4EncoderPath::HardwareTransformsEnabled;
      return writer;
    }

    writer.reset();
    ::DeleteFileW(partialPath.c_str());
    writer = writerFactory_();
    if (!writer) {
      result = E_OUTOFMEMORY;
      return nullptr;
    }
    result = writer->open(config, partialPath, false);
    if (SUCCEEDED(result)) {
      path = Mp4EncoderPath::SoftwareOnly;
      return writer;
    }
    writer.reset();
    return nullptr;
  }

  void run() {
    ::SetThreadDescription(::GetCurrentThread(), L"RemoteCam MP4 recorder");

    Mp4RecorderConfig config;
    std::wstring partialPath;
    {
      std::lock_guard<std::mutex> lock(queueMutex_);
      config = config_;
      partialPath = partialPath_;
    }

    HRESULT hr = S_OK;
    Mp4EncoderPath encoderPath = Mp4EncoderPath::Unknown;
    std::unique_ptr<detail::IMp4Writer> writer =
        openWriter(config, partialPath, encoderPath, hr);
    if (!writer) {
      discardAndFail(hr, 0, partialPath);
      return;
    }
    replaceSnapshot([&](Mp4RecorderSnapshot& current) {
      current.state = Mp4RecorderState::Recording;
      current.encoderPath = encoderPath;
    }, true);

    const rcwin::Nv12Layout layout =
        rcwin::nv12Layout(static_cast<int>(config.width),
                          static_cast<int>(config.height));
    std::vector<uint8_t> nv12(layout.totalSize);
    MonotonicTimeline timeline(config);

    for (;;) {
      std::shared_ptr<const BgraPreviewFrame> frame;
      size_t queued = 0;
      {
        std::unique_lock<std::mutex> lock(queueMutex_);
        queueWake_.wait(lock, [&] { return stopRequested_ || !queue_.empty(); });
        if (queue_.empty()) {
          if (stopRequested_) break;
          continue;
        }
        frame = std::move(queue_.front());
        queue_.pop_front();
        queued = queue_.size();
        replaceSnapshot([&](Mp4RecorderSnapshot& current) {
          current.queuedFrames = queued;
        }, false);
      }

      hr = bgraToNv12(frame->pixels.data(), frame->pixels.size(), frame->stride,
                      frame->width, frame->height, nv12.data(), nv12.size(),
                      static_cast<uint32_t>(layout.stride));
      if (FAILED(hr)) {
        writer.reset();
        discardAndFail(hr, 1, partialPath);
        return;
      }
      const LONGLONG timestamp = timeline.next(frame->ptsMicros);
      hr = writer->writeNv12(nv12.data(), nv12.size(), timestamp,
                             timeline.duration());
      if (FAILED(hr)) {
        writer.reset();
        discardAndFail(hr, 1, partialPath);
        return;
      }
      replaceSnapshot([&](Mp4RecorderSnapshot& current) {
        ++current.writtenFrames;
        const uint64_t sampleEnd =
            static_cast<uint64_t>(timestamp) +
            static_cast<uint64_t>(timeline.duration());
        current.duration100ns = sampleEnd;
      }, false);
    }

    replaceSnapshot([](Mp4RecorderSnapshot& current) {
      current.state = Mp4RecorderState::Finalizing;
    }, true);
    hr = writer->finalize();
    writer.reset();
    if (FAILED(hr)) {
      discardAndFail(hr, 0, partialPath);
      return;
    }

    if (!::MoveFileExW(partialPath.c_str(), config.outputPath.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
      hr = rcwin::hrFromLastError();
      // Finalize succeeded, so preserve the playable partial and publish its path.
      replaceSnapshot([&](Mp4RecorderSnapshot& current) {
        current.state = Mp4RecorderState::Failed;
        current.error = hr;
        current.queuedFrames = 0;
      }, true);
      return;
    }

    replaceSnapshot([](Mp4RecorderSnapshot& current) {
      current.state = Mp4RecorderState::Completed;
      current.error = S_OK;
      current.queuedFrames = 0;
      current.partialPath.clear();
    }, true);
  }

  detail::Mp4WriterFactory writerFactory_;

  mutable std::mutex statusMutex_;
  Mp4RecorderSnapshot status_;
  Observer observer_;

  std::mutex lifecycleMutex_;
  std::mutex queueMutex_;
  std::condition_variable queueWake_;
  std::deque<std::shared_ptr<const BgraPreviewFrame>> queue_;
  Mp4RecorderConfig config_;
  std::wstring partialPath_;
  bool sessionActive_ = false;
  bool stopRequested_ = false;
  std::thread worker_;
};

}  // namespace

std::unique_ptr<IMp4Recorder> createMp4Recorder() {
  return detail::createMp4RecorderWithWriterFactory(
      [] { return detail::createMfMp4Writer(); });
}

namespace detail {

std::unique_ptr<IMp4Recorder> createMp4RecorderWithWriterFactory(
    Mp4WriterFactory factory) {
  if (!factory) return nullptr;
  return std::unique_ptr<IMp4Recorder>(
      new (std::nothrow) Mp4Recorder(std::move(factory)));
}

}  // namespace detail

}  // namespace rcplatform
