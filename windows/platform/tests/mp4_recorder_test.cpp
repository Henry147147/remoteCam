#include <windows.h>

#include <mferror.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mp4_recorder_internal.h"
#include "rcplatform/mp4_recorder.h"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool value, const std::string& what) {
  ++g_checks;
  if (!value) {
    ++g_failures;
    std::printf("  FAIL: %s\n", what.c_str());
  }
}

void checkHr(HRESULT got, HRESULT want, const std::string& what) {
  ++g_checks;
  if (got != want) {
    ++g_failures;
    std::printf("  FAIL: %s (got 0x%08lX, want 0x%08lX)\n", what.c_str(),
                static_cast<unsigned long>(got), static_cast<unsigned long>(want));
  }
}

std::wstring testDirectory() {
  std::vector<wchar_t> buffer(MAX_PATH + 1u);
  const DWORD length = ::GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
  if (length == 0 || length >= buffer.size()) return {};
  std::wstring path(buffer.data(), length);
  path += L"RemoteCam.RecorderTests.";
  path += std::to_wstring(::GetCurrentProcessId());
  ::CreateDirectoryW(path.c_str(), nullptr);
  return path;
}

void writeText(const std::wstring& path, const char* text) {
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  ::WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
  ::CloseHandle(file);
}

std::string readText(const std::wstring& path) {
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  const DWORD size = ::GetFileSize(file, nullptr);
  std::string bytes(size, '\0');
  DWORD read = 0;
  if (size != 0 && !::ReadFile(file, bytes.data(), size, &read, nullptr)) read = 0;
  ::CloseHandle(file);
  bytes.resize(read);
  return bytes;
}

uint64_t fileSize(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA attributes{};
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) return 0;
  ULARGE_INTEGER size;
  size.HighPart = attributes.nFileSizeHigh;
  size.LowPart = attributes.nFileSizeLow;
  return size.QuadPart;
}

bool isH264Mp4(const std::wstring& path) {
  Microsoft::WRL::ComPtr<IMFSourceReader> reader;
  if (FAILED(::MFCreateSourceReaderFromURL(path.c_str(), nullptr, &reader))) return false;
  Microsoft::WRL::ComPtr<IMFMediaType> type;
  constexpr DWORD firstVideo =
      static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
  if (FAILED(reader->GetNativeMediaType(firstVideo, 0, &type))) return false;
  GUID major = GUID_NULL;
  GUID subtype = GUID_NULL;
  return SUCCEEDED(type->GetGUID(MF_MT_MAJOR_TYPE, &major)) &&
         SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
         major == MFMediaType_Video && subtype == MFVideoFormat_H264;
}

std::shared_ptr<const rcplatform::BgraPreviewFrame> makeFrame(
    uint32_t width, uint32_t height, uint64_t ptsMicros, uint8_t value = 64) {
  auto frame = std::make_shared<rcplatform::BgraPreviewFrame>();
  frame->width = width;
  frame->height = height;
  frame->stride = width * 4u;
  frame->ptsMicros = ptsMicros;
  frame->pixels.resize(static_cast<size_t>(frame->stride) * height);
  for (size_t index = 0; index < frame->pixels.size(); index += 4u) {
    frame->pixels[index] = value;
    frame->pixels[index + 1u] = static_cast<uint8_t>(value / 2u);
    frame->pixels[index + 2u] = static_cast<uint8_t>(255u - value);
    frame->pixels[index + 3u] = 255;
  }
  return frame;
}

bool waitForState(rcplatform::IMp4Recorder& recorder,
                  rcplatform::Mp4RecorderState wanted,
                  std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (recorder.snapshot().state == wanted) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return recorder.snapshot().state == wanted;
}

struct FakeWriterState {
  std::mutex mutex;
  std::condition_variable wake;
  std::vector<bool> hardwareAttempts;
  std::vector<LONGLONG> timestamps;
  std::vector<LONGLONG> durations;
  std::wstring partialPath;
  bool failHardwareOpen = false;
  HRESULT softwareOpenResult = S_OK;
  HRESULT writeResult = S_OK;
  HRESULT finalizeResult = S_OK;
  bool blockFirstWrite = false;
  bool writeEntered = false;
  bool releaseWrite = false;
};

class FakeWriter final : public rcplatform::detail::IMp4Writer {
 public:
  explicit FakeWriter(std::shared_ptr<FakeWriterState> state)
      : state_(std::move(state)) {}

  HRESULT open(const rcplatform::Mp4RecorderConfig&, const std::wstring& partialPath,
               bool enableHardwareTransforms) override {
    HRESULT result = S_OK;
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->hardwareAttempts.push_back(enableHardwareTransforms);
      state_->partialPath = partialPath;
      if (enableHardwareTransforms && state_->failHardwareOpen) result = E_FAIL;
      if (!enableHardwareTransforms) result = state_->softwareOpenResult;
    }
    // Deliberately leave a file even on open failure. The recorder owns cleanup before
    // retrying and the tests prove a failed hardware attempt cannot contaminate the
    // software attempt or final output.
    writeText(partialPath, SUCCEEDED(result) ? "new" : "failed");
    return result;
  }

  HRESULT writeNv12(const uint8_t*, size_t, LONGLONG timestamp100ns,
                    LONGLONG duration100ns) override {
    std::unique_lock<std::mutex> lock(state_->mutex);
    if (state_->blockFirstWrite && state_->timestamps.empty()) {
      state_->writeEntered = true;
      state_->wake.notify_all();
      state_->wake.wait(lock, [&] { return state_->releaseWrite; });
    }
    state_->timestamps.push_back(timestamp100ns);
    state_->durations.push_back(duration100ns);
    return state_->writeResult;
  }

  HRESULT finalize() override {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->finalizeResult;
  }

 private:
  std::shared_ptr<FakeWriterState> state_;
};

rcplatform::detail::Mp4WriterFactory fakeFactory(
    const std::shared_ptr<FakeWriterState>& state) {
  return [state] { return std::make_unique<FakeWriter>(state); };
}

rcplatform::Mp4RecorderConfig tinyConfig(const std::wstring& output) {
  rcplatform::Mp4RecorderConfig config;
  config.outputPath = output;
  config.width = 4;
  config.height = 2;
  config.fpsNumerator = 30;
  config.fpsDenominator = 1;
  config.bitrateBitsPerSecond = 100'000;
  config.queueCapacity = 8;
  return config;
}

void testValidation(const std::wstring& directory) {
  std::printf("Recorder rejects invalid sessions and frames\n");
  auto state = std::make_shared<FakeWriterState>();
  auto recorder = rcplatform::detail::createMp4RecorderWithWriterFactory(
      fakeFactory(state));
  check(static_cast<bool>(recorder), "test recorder is created");
  if (!recorder) return;

  auto config = tinyConfig(directory + L"\\invalid.avi");
  checkHr(recorder->start(config), E_INVALIDARG, "non-MP4 destination is rejected");
  config.outputPath = directory + L"\\invalid.mp4";
  config.width = 3;
  checkHr(recorder->start(config), E_INVALIDARG, "odd BGRA geometry is rejected");
  config.width = 4;
  config.queueCapacity = 0;
  checkHr(recorder->start(config), E_INVALIDARG, "an unbounded/empty queue is rejected");
  checkHr(recorder->enqueue(nullptr), E_POINTER, "null frames are rejected");
  checkHr(recorder->enqueue(makeFrame(4, 2, 0)), MF_E_NOTACCEPTING,
          "frames cannot queue before start");
  std::lock_guard<std::mutex> lock(state->mutex);
  check(state->hardwareAttempts.empty(), "validation fails before creating an encoder");
}

void testFallbackTimelineAndAtomicReplace(const std::wstring& directory) {
  std::printf("Hardware fallback, monotonic timestamps, and atomic publication\n");
  const std::wstring output = directory + L"\\fallback.mp4";
  writeText(output, "old");

  auto state = std::make_shared<FakeWriterState>();
  state->failHardwareOpen = true;
  auto recorder = rcplatform::detail::createMp4RecorderWithWriterFactory(
      fakeFactory(state));
  checkHr(recorder->start(tinyConfig(output)), S_OK, "valid recording starts asynchronously");
  check(waitForState(*recorder, rcplatform::Mp4RecorderState::Recording),
        "software fallback reaches Recording");

  checkHr(recorder->enqueue(makeFrame(4, 2, 1'000)), S_OK, "first frame queues");
  checkHr(recorder->enqueue(makeFrame(4, 2, 1'000)), S_OK, "duplicate PTS queues");
  checkHr(recorder->enqueue(makeFrame(4, 2, 900)), S_OK, "regressing PTS queues");
  checkHr(recorder->enqueue(makeFrame(4, 2, 200'000)), S_OK, "later PTS queues");
  checkHr(recorder->stop(), S_OK, "stop drains, finalizes, and publishes");

  const rcplatform::Mp4RecorderSnapshot snapshot = recorder->snapshot();
  check(snapshot.state == rcplatform::Mp4RecorderState::Completed,
        "successful finalization reaches Completed");
  check(snapshot.encoderPath == rcplatform::Mp4EncoderPath::SoftwareOnly,
        "snapshot reports the software-only retry path");
  check(snapshot.submittedFrames == 4 && snapshot.writtenFrames == 4 &&
            snapshot.droppedFrames == 0,
        "all non-overflowing frames are accounted for");
  check(snapshot.duration100ns == 2'323'333,
        "status exposes the muxed timeline duration");
  check(snapshot.partialPath.empty(), "successful atomic rename clears the partial path");
  check(readText(output) == "new", "final output atomically replaces the prior file");

  std::lock_guard<std::mutex> lock(state->mutex);
  check(state->hardwareAttempts == std::vector<bool>({true, false}),
        "hardware-enabled transforms are attempted before software-only transforms");
  check(state->timestamps.size() == 4, "writer receives every frame timestamp");
  if (state->timestamps.size() == 4) {
    check(state->timestamps[0] == 0, "recording timeline starts at zero");
    check(state->timestamps[1] == 333'333 && state->timestamps[2] == 666'666,
          "duplicate and regressing PTS advance by one frame duration");
    check(state->timestamps[3] == 1'990'000,
          "a valid later source gap is preserved");
    check(state->durations ==
              std::vector<LONGLONG>({333'333, 333'333, 333'333, 333'333}),
          "every sample carries the configured frame duration");
  }
  check(::GetFileAttributesW(state->partialPath.c_str()) == INVALID_FILE_ATTRIBUTES,
        "hardware attempt and successful partial files leave no residue");
}

void testBoundedQueueDropsOldest(const std::wstring& directory) {
  std::printf("Bounded queue drops the oldest frame without blocking the caller\n");
  const std::wstring output = directory + L"\\bounded.mp4";
  auto state = std::make_shared<FakeWriterState>();
  state->blockFirstWrite = true;
  auto recorder = rcplatform::detail::createMp4RecorderWithWriterFactory(
      fakeFactory(state));
  auto config = tinyConfig(output);
  config.queueCapacity = 2;
  checkHr(recorder->start(config), S_OK, "bounded recording starts");
  check(waitForState(*recorder, rcplatform::Mp4RecorderState::Recording),
        "bounded recording becomes ready");
  checkHr(recorder->enqueue(makeFrame(4, 2, 0)), S_OK, "worker takes the first frame");
  {
    std::unique_lock<std::mutex> lock(state->mutex);
    check(state->wake.wait_for(lock, std::chrono::seconds(5),
                               [&] { return state->writeEntered; }),
          "fake writer blocks with one frame in flight");
  }
  checkHr(recorder->enqueue(makeFrame(4, 2, 33'333)), S_OK, "queue slot one fills");
  checkHr(recorder->enqueue(makeFrame(4, 2, 66'666)), S_OK, "queue slot two fills");
  checkHr(recorder->enqueue(makeFrame(4, 2, 99'999)), S_FALSE,
          "overflow accepts newest frame and reports an old-frame drop");
  {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->releaseWrite = true;
  }
  state->wake.notify_all();
  checkHr(recorder->stop(), S_OK, "bounded recording drains after encoder resumes");

  const rcplatform::Mp4RecorderSnapshot snapshot = recorder->snapshot();
  check(snapshot.submittedFrames == 4 && snapshot.writtenFrames == 3 &&
            snapshot.droppedFrames == 1,
        "bounded queue counters reconcile exactly");
  std::lock_guard<std::mutex> lock(state->mutex);
  check(state->timestamps.size() == 3, "the in-flight and two retained frames are written");
  if (state->timestamps.size() == 3) {
    check(state->timestamps[0] < state->timestamps[1] &&
              state->timestamps[1] < state->timestamps[2],
          "timestamps remain strictly monotonic across a queue drop");
  }
}

void testFinalizeFailureProtectsDestination(const std::wstring& directory) {
  std::printf("Muxer failure never replaces the destination with a partial file\n");
  const std::wstring output = directory + L"\\finalize-failure.mp4";
  writeText(output, "old");
  auto state = std::make_shared<FakeWriterState>();
  state->finalizeResult = E_FAIL;
  auto recorder = rcplatform::detail::createMp4RecorderWithWriterFactory(
      fakeFactory(state));
  checkHr(recorder->start(tinyConfig(output)), S_OK, "failure test starts");
  check(waitForState(*recorder, rcplatform::Mp4RecorderState::Recording),
        "failure test reaches Recording");
  checkHr(recorder->enqueue(makeFrame(4, 2, 0)), S_OK, "failure test frame queues");
  checkHr(recorder->stop(), E_FAIL, "Finalize error reaches the caller");
  const rcplatform::Mp4RecorderSnapshot snapshot = recorder->snapshot();
  check(snapshot.state == rcplatform::Mp4RecorderState::Failed &&
            snapshot.error == E_FAIL,
        "Finalize error is exposed in recorder status");
  check(readText(output) == "old", "existing destination survives Finalize failure");
  check(snapshot.partialPath.empty(), "unplayable partial is removed after muxer failure");
  std::lock_guard<std::mutex> lock(state->mutex);
  check(::GetFileAttributesW(state->partialPath.c_str()) == INVALID_FILE_ATTRIBUTES,
        "failed muxer leaves no incomplete file behind");
}

void testRenameFailurePreservesPlayablePartial(const std::wstring& directory) {
  std::printf("Atomic rename failure preserves the finalized partial for recovery\n");
  const std::wstring output = directory + L"\\rename-failure.mp4";
  writeText(output, "old");
  HANDLE destinationLock = ::CreateFileW(output.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                         nullptr, OPEN_EXISTING,
                                         FILE_ATTRIBUTE_NORMAL, nullptr);
  check(destinationLock != INVALID_HANDLE_VALUE,
        "destination is locked against delete/replace for the test");
  if (destinationLock == INVALID_HANDLE_VALUE) return;

  auto state = std::make_shared<FakeWriterState>();
  auto recorder = rcplatform::detail::createMp4RecorderWithWriterFactory(
      fakeFactory(state));
  checkHr(recorder->start(tinyConfig(output)), S_OK, "rename failure test starts");
  check(waitForState(*recorder, rcplatform::Mp4RecorderState::Recording),
        "rename failure test reaches Recording");
  checkHr(recorder->enqueue(makeFrame(4, 2, 0)), S_OK,
          "rename failure test frame queues");
  const HRESULT stopResult = recorder->stop();
  check(stopResult == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) ||
            stopResult == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED),
        "atomic rename exposes the destination lock error");
  const rcplatform::Mp4RecorderSnapshot snapshot = recorder->snapshot();
  check(snapshot.state == rcplatform::Mp4RecorderState::Failed &&
            snapshot.error == stopResult,
        "rename failure is visible in status");
  check(!snapshot.partialPath.empty() && readText(snapshot.partialPath) == "new",
        "finalized partial remains available for recovery");
  check(readText(output) == "old", "locked destination remains untouched");

  ::CloseHandle(destinationLock);
  ::DeleteFileW(snapshot.partialPath.c_str());
}

class AlwaysFailWriter final : public rcplatform::detail::IMp4Writer {
 public:
  HRESULT open(const rcplatform::Mp4RecorderConfig&, const std::wstring&, bool) override {
    return E_FAIL;
  }
  HRESULT writeNv12(const uint8_t*, size_t, LONGLONG, LONGLONG) override {
    return E_UNEXPECTED;
  }
  HRESULT finalize() override { return E_UNEXPECTED; }
};

void testRealSoftwareEncoder(const std::wstring& directory) {
  std::printf("Media Foundation software H.264 Sink Writer smoke\n");
  const std::wstring output = directory + L"\\mf-software.mp4";
  std::atomic<unsigned int> factoryCalls{0};
  auto recorder = rcplatform::detail::createMp4RecorderWithWriterFactory([&] {
    if (factoryCalls.fetch_add(1, std::memory_order_relaxed) == 0) {
      return std::unique_ptr<rcplatform::detail::IMp4Writer>(
          std::make_unique<AlwaysFailWriter>());
    }
    return rcplatform::detail::createMfMp4Writer();
  });

  rcplatform::Mp4RecorderConfig config;
  config.outputPath = output;
  config.width = 320;
  config.height = 240;
  config.fpsNumerator = 30;
  config.fpsDenominator = 1;
  config.bitrateBitsPerSecond = 1'000'000;
  config.queueCapacity = 16;
  checkHr(recorder->start(config), S_OK, "real software encoder starts asynchronously");
  const bool recording = waitForState(*recorder, rcplatform::Mp4RecorderState::Recording,
                                      std::chrono::seconds(10));
  check(recording, "software-only MF Sink Writer reaches Recording");
  if (recording) {
    for (uint64_t index = 0; index < 12; ++index) {
      check(SUCCEEDED(recorder->enqueue(makeFrame(
                config.width, config.height, index * 33'333u,
                static_cast<uint8_t>(32u + index * 8u)))),
            "real encoder frame queues");
    }
    checkHr(recorder->stop(), S_OK, "real software encoder finalizes");
    const rcplatform::Mp4RecorderSnapshot snapshot = recorder->snapshot();
    check(snapshot.state == rcplatform::Mp4RecorderState::Completed,
          "real encoder publishes a completed MP4");
    check(snapshot.encoderPath == rcplatform::Mp4EncoderPath::SoftwareOnly,
          "real fallback disables hardware transforms");
    check(fileSize(output) > 128u, "real MP4 has a non-empty muxed payload");
    check(isH264Mp4(output), "final MP4 advertises a native H.264 video stream");
  } else {
    const auto snapshot = recorder->snapshot();
    std::printf("  MF startup error: 0x%08lX\n",
                static_cast<unsigned long>(snapshot.error));
    recorder->stop();
  }
}

}  // namespace

int main() {
  const HRESULT coStartup = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(coStartup)) {
    std::printf("CoInitializeEx failed: 0x%08lX\n",
                static_cast<unsigned long>(coStartup));
    return 1;
  }
  const HRESULT mfStartup = ::MFStartup(MF_VERSION, MFSTARTUP_FULL);
  if (FAILED(mfStartup)) {
    std::printf("MFStartup failed: 0x%08lX\n",
                static_cast<unsigned long>(mfStartup));
    ::CoUninitialize();
    return 1;
  }

  const std::wstring directory = testDirectory();
  if (directory.empty()) {
    std::printf("Could not create recorder test directory\n");
    ::MFShutdown();
    ::CoUninitialize();
    return 1;
  }

  testValidation(directory);
  testFallbackTimelineAndAtomicReplace(directory);
  testBoundedQueueDropsOldest(directory);
  testFinalizeFailureProtectsDestination(directory);
  testRenameFailurePreservesPlayablePartial(directory);
  testRealSoftwareEncoder(directory);

  for (const wchar_t* name : {L"fallback.mp4", L"bounded.mp4",
                              L"finalize-failure.mp4", L"rename-failure.mp4",
                              L"mf-software.mp4"}) {
    const std::wstring path = directory + L"\\" + name;
    ::DeleteFileW(path.c_str());
  }
  ::RemoveDirectoryW(directory.c_str());

  ::MFShutdown();
  ::CoUninitialize();

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
