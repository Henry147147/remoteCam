#ifndef RCPLATFORM_MP4_RECORDER_INTERNAL_H
#define RCPLATFORM_MP4_RECORDER_INTERNAL_H

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "rcplatform/mp4_recorder.h"

namespace rcplatform::detail {

// Narrow dependency seam used by the recorder tests. The production implementation
// is the Media Foundation Sink Writer below; keeping file publication and queue policy
// above this seam makes failure/fallback behavior deterministic to exercise.
class IMp4Writer {
 public:
  virtual ~IMp4Writer() = default;
  virtual HRESULT open(const Mp4RecorderConfig& config,
                       const std::wstring& partialPath,
                       bool enableHardwareTransforms) = 0;
  virtual HRESULT writeNv12(const uint8_t* bytes, size_t size,
                            LONGLONG timestamp100ns,
                            LONGLONG duration100ns) = 0;
  virtual HRESULT finalize() = 0;
};

using Mp4WriterFactory = std::function<std::unique_ptr<IMp4Writer>()>;

std::unique_ptr<IMp4Writer> createMfMp4Writer();
std::unique_ptr<IMp4Recorder> createMp4RecorderWithWriterFactory(
    Mp4WriterFactory factory);

}  // namespace rcplatform::detail

#endif  // RCPLATFORM_MP4_RECORDER_INTERNAL_H
