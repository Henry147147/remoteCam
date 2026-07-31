#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <wrl/client.h>

#include <array>
#include <cstdio>
#include <set>
#include <string>
#include <tuple>

#include "media_format.h"
#include "rcwin/shm_ring.h"

namespace {

using Microsoft::WRL::ComPtr;

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

void testLadderShape() {
  std::printf("NV12 media-type ladder shape\n");
  check(rcvcam::kVideoFormats.size() == 12, "six resolutions times two rates");
  check(rcvcam::kDefaultVideoFormat == rcvcam::VideoFormat{1920, 1080, 30, 1},
        "1080p30 remains the conservative default");

  const std::array<std::pair<UINT32, UINT32>, 6> resolutions = {{
      {3840, 2160}, {2560, 1440}, {1920, 1080},
      {1280, 720},  {960, 540},   {640, 480},
  }};
  std::set<std::tuple<UINT32, UINT32, UINT32, UINT32>> unique;
  for (const rcvcam::VideoFormat& format : rcvcam::kVideoFormats) {
    unique.emplace(format.width, format.height, format.fpsNumerator,
                   format.fpsDenominator);
    check(format.width % 2 == 0 && format.height % 2 == 0,
          "every NV12 geometry is even");
    check(format.width <= rcwin::kRingMaxWidth && format.height <= rcwin::kRingMaxHeight,
          "every geometry fits one frame-ring slot");
    check(format.fpsDenominator == 1 &&
              (format.fpsNumerator == 30 || format.fpsNumerator == 60),
          "each media type is 30 or 60 fps");
  }
  check(unique.size() == rcvcam::kVideoFormats.size(), "all ladder entries are unique");

  for (const auto& [width, height] : resolutions) {
    check(unique.contains({width, height, 30, 1}), "resolution has a 30 fps entry");
    check(unique.contains({width, height, 60, 1}), "resolution has a 60 fps entry");
  }
  check(rcvcam::frameDuration100ns({1920, 1080, 30, 1}) == 333333,
        "30 fps duration uses Media Foundation 100 ns units");
  check(rcvcam::frameDuration100ns({1920, 1080, 60, 1}) == 166666,
        "60 fps duration uses Media Foundation 100 ns units");
}

void testMediaTypeRoundTrips() {
  std::printf("Media types carry exact geometry, rate and NV12 attributes\n");
  for (const rcvcam::VideoFormat& expected : rcvcam::kVideoFormats) {
    ComPtr<IMFMediaType> type;
    checkHr(rcvcam::createVideoMediaType(expected, &type), S_OK,
            "supported format creates a media type");
    if (!type) continue;

    rcvcam::VideoFormat parsed;
    checkHr(rcvcam::videoFormatFromMediaType(type.Get(), parsed), S_OK,
            "created media type parses as supported");
    check(parsed == expected, "media-type round trip is exact");

    GUID major = GUID_NULL;
    GUID subtype = GUID_NULL;
    UINT32 stride = 0, sampleSize = 0, fixed = 0, independent = 0, interlace = 0;
    checkHr(type->GetGUID(MF_MT_MAJOR_TYPE, &major), S_OK, "major type is present");
    checkHr(type->GetGUID(MF_MT_SUBTYPE, &subtype), S_OK, "subtype is present");
    check(major == MFMediaType_Video && subtype == MFVideoFormat_NV12,
          "the ladder advertises video/NV12 only");
    checkHr(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride), S_OK,
            "default stride is explicit");
    checkHr(type->GetUINT32(MF_MT_SAMPLE_SIZE, &sampleSize), S_OK,
            "sample size is explicit");
    checkHr(type->GetUINT32(MF_MT_FIXED_SIZE_SAMPLES, &fixed), S_OK,
            "fixed-size samples are explicit");
    checkHr(type->GetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, &independent), S_OK,
            "sample independence is explicit");
    checkHr(type->GetUINT32(MF_MT_INTERLACE_MODE, &interlace), S_OK,
            "progressive mode is explicit");
    check(stride == expected.width, "NV12 stride matches the visible width");
    check(sampleSize == expected.width * expected.height * 3u / 2u,
          "NV12 sample size matches the geometry");
    check(fixed == TRUE && independent == TRUE &&
              interlace == MFVideoInterlace_Progressive,
          "fixed, independent, progressive attributes have the expected values");
  }
}

void testUnsupportedMediaTypesAreRejected() {
  std::printf("Unsupported media types cannot enter the active stream\n");

  ComPtr<IMFMediaType> type;
  checkHr(rcvcam::createVideoMediaType({800, 600, 30, 1}, &type), E_INVALIDARG,
          "factory rejects a geometry outside the fixed ladder");
  check(!type, "failed factory does not return a partial media type");

  checkHr(rcvcam::createVideoMediaType(rcvcam::kDefaultVideoFormat, &type), S_OK,
          "baseline media type is available for corruption tests");
  if (!type) return;
  rcvcam::VideoFormat parsed;

  checkHr(::MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, 800, 600), S_OK,
          "test changes the geometry");
  checkHr(rcvcam::videoFormatFromMediaType(type.Get(), parsed), MF_E_INVALIDMEDIATYPE,
          "unsupported geometry is rejected at Start validation");

  checkHr(rcvcam::createVideoMediaType(rcvcam::kDefaultVideoFormat, &type), S_OK,
          "fresh baseline for subtype rejection");
  checkHr(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32), S_OK,
          "test changes the subtype");
  checkHr(rcvcam::videoFormatFromMediaType(type.Get(), parsed), MF_E_INVALIDMEDIATYPE,
          "non-NV12 media type is rejected");

  checkHr(rcvcam::createVideoMediaType(rcvcam::kDefaultVideoFormat, &type), S_OK,
          "fresh baseline for missing-rate rejection");
  checkHr(type->DeleteItem(MF_MT_FRAME_RATE), S_OK, "test removes frame rate");
  check(FAILED(rcvcam::videoFormatFromMediaType(type.Get(), parsed)),
        "missing frame rate is rejected");
}

}  // namespace

int main() {
  const HRESULT startup = ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
  if (FAILED(startup)) {
    std::printf("MFStartup failed: 0x%08lX\n", static_cast<unsigned long>(startup));
    return 1;
  }

  testLadderShape();
  testMediaTypeRoundTrips();
  testUnsupportedMediaTypesAreRejected();

  ::MFShutdown();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
