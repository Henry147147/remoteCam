#include "mp4_recorder_internal.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cstring>
#include <limits>
#include <new>

#include "rcwin/hr.h"

namespace rcplatform::detail {
namespace {

using Microsoft::WRL::ComPtr;

class MfMp4Writer final : public IMp4Writer {
 public:
  ~MfMp4Writer() override {
    writer_.Reset();
    if (mfStarted_) ::MFShutdown();
    if (comInitialized_) ::CoUninitialize();
  }

  HRESULT open(const Mp4RecorderConfig& config,
               const std::wstring& partialPath,
               bool enableHardwareTransforms) override {
    if (writer_ || mfStarted_ || partialPath.empty()) return E_UNEXPECTED;

    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) return hr;
    comInitialized_ = true;

    hr = ::MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) return hr;
    mfStarted_ = true;

    const uint64_t sampleBytes =
        static_cast<uint64_t>(config.width) * config.height * 3u / 2u;
    if (sampleBytes == 0 || sampleBytes > (std::numeric_limits<DWORD>::max)()) {
      return E_INVALIDARG;
    }
    expectedSampleBytes_ = static_cast<DWORD>(sampleBytes);

    ComPtr<IMFAttributes> attributes;
    RC_RETURN_IF_FAILED(::MFCreateAttributes(&attributes, 3));
    RC_RETURN_IF_FAILED(attributes->SetUINT32(
        MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
        enableHardwareTransforms ? TRUE : FALSE));
    RC_RETURN_IF_FAILED(
        attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE));
    RC_RETURN_IF_FAILED(attributes->SetUINT32(MF_LOW_LATENCY, TRUE));

    RC_RETURN_IF_FAILED(::MFCreateSinkWriterFromURL(
        partialPath.c_str(), nullptr, attributes.Get(), &writer_));

    ComPtr<IMFMediaType> outputType;
    RC_RETURN_IF_FAILED(::MFCreateMediaType(&outputType));
    RC_RETURN_IF_FAILED(outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    RC_RETURN_IF_FAILED(outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264));
    RC_RETURN_IF_FAILED(outputType->SetUINT32(
        MF_MT_AVG_BITRATE, config.bitrateBitsPerSecond));
    RC_RETURN_IF_FAILED(outputType->SetUINT32(
        MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    RC_RETURN_IF_FAILED(::MFSetAttributeSize(
        outputType.Get(), MF_MT_FRAME_SIZE, config.width, config.height));
    RC_RETURN_IF_FAILED(::MFSetAttributeRatio(
        outputType.Get(), MF_MT_FRAME_RATE, config.fpsNumerator,
        config.fpsDenominator));
    RC_RETURN_IF_FAILED(
        ::MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    RC_RETURN_IF_FAILED(writer_->AddStream(outputType.Get(), &streamIndex_));

    ComPtr<IMFMediaType> inputType;
    RC_RETURN_IF_FAILED(::MFCreateMediaType(&inputType));
    RC_RETURN_IF_FAILED(inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
    RC_RETURN_IF_FAILED(inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
    RC_RETURN_IF_FAILED(inputType->SetUINT32(
        MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
    RC_RETURN_IF_FAILED(inputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
    RC_RETURN_IF_FAILED(inputType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE));
    RC_RETURN_IF_FAILED(inputType->SetUINT32(MF_MT_DEFAULT_STRIDE, config.width));
    RC_RETURN_IF_FAILED(
        inputType->SetUINT32(MF_MT_SAMPLE_SIZE, expectedSampleBytes_));
    RC_RETURN_IF_FAILED(::MFSetAttributeSize(
        inputType.Get(), MF_MT_FRAME_SIZE, config.width, config.height));
    RC_RETURN_IF_FAILED(::MFSetAttributeRatio(
        inputType.Get(), MF_MT_FRAME_RATE, config.fpsNumerator,
        config.fpsDenominator));
    RC_RETURN_IF_FAILED(
        ::MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
    RC_RETURN_IF_FAILED(
        writer_->SetInputMediaType(streamIndex_, inputType.Get(), nullptr));
    return writer_->BeginWriting();
  }

  HRESULT writeNv12(const uint8_t* bytes, size_t size,
                    LONGLONG timestamp100ns,
                    LONGLONG duration100ns) override {
    if (!writer_) return MF_E_NOT_INITIALIZED;
    if (!bytes || size != expectedSampleBytes_ || timestamp100ns < 0 ||
        duration100ns <= 0) {
      return E_INVALIDARG;
    }

    ComPtr<IMFMediaBuffer> buffer;
    RC_RETURN_IF_FAILED(::MFCreateMemoryBuffer(expectedSampleBytes_, &buffer));
    BYTE* destination = nullptr;
    DWORD maximumLength = 0;
    RC_RETURN_IF_FAILED(buffer->Lock(&destination, &maximumLength, nullptr));
    if (!destination || maximumLength < expectedSampleBytes_) {
      buffer->Unlock();
      return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    }
    std::memcpy(destination, bytes, expectedSampleBytes_);
    HRESULT hr = buffer->Unlock();
    if (FAILED(hr)) return hr;
    RC_RETURN_IF_FAILED(buffer->SetCurrentLength(expectedSampleBytes_));

    ComPtr<IMFSample> sample;
    RC_RETURN_IF_FAILED(::MFCreateSample(&sample));
    RC_RETURN_IF_FAILED(sample->AddBuffer(buffer.Get()));
    RC_RETURN_IF_FAILED(sample->SetSampleTime(timestamp100ns));
    RC_RETURN_IF_FAILED(sample->SetSampleDuration(duration100ns));
    return writer_->WriteSample(streamIndex_, sample.Get());
  }

  HRESULT finalize() override {
    if (!writer_) return MF_E_NOT_INITIALIZED;
    const HRESULT hr = writer_->Finalize();
    // Releasing the sink closes its byte stream. The recorder renames only after this
    // object is reset, so MoveFileEx never races an encoder-owned file handle.
    writer_.Reset();
    return hr;
  }

 private:
  bool comInitialized_ = false;
  bool mfStarted_ = false;
  DWORD streamIndex_ = 0;
  DWORD expectedSampleBytes_ = 0;
  ComPtr<IMFSinkWriter> writer_;
};

}  // namespace

std::unique_ptr<IMp4Writer> createMfMp4Writer() {
  return std::unique_ptr<IMp4Writer>(new (std::nothrow) MfMp4Writer());
}

}  // namespace rcplatform::detail
