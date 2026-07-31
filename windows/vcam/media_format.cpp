#include "media_format.h"

#include <mfapi.h>
#include <mferror.h>
#include <wrl/client.h>

#include "rcwin/hr.h"

using Microsoft::WRL::ComPtr;

namespace rcvcam {

HRESULT createVideoMediaType(const VideoFormat& format, IMFMediaType** out) {
  RC_RETURN_IF_NULL(out);
  *out = nullptr;
  RC_RETURN_HR_IF(!isSupportedVideoFormat(format), E_INVALIDARG);

  ComPtr<IMFMediaType> type;
  RC_RETURN_IF_FAILED(::MFCreateMediaType(&type));
  RC_RETURN_IF_FAILED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
  RC_RETURN_IF_FAILED(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12));
  RC_RETURN_IF_FAILED(
      type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive));
  RC_RETURN_IF_FAILED(type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE));
  RC_RETURN_IF_FAILED(type->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE));
  RC_RETURN_IF_FAILED(
      ::MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, format.width, format.height));
  RC_RETURN_IF_FAILED(::MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE,
                                            format.fpsNumerator,
                                            format.fpsDenominator));
  RC_RETURN_IF_FAILED(::MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1));
  RC_RETURN_IF_FAILED(type->SetUINT32(MF_MT_DEFAULT_STRIDE, format.width));
  RC_RETURN_IF_FAILED(
      type->SetUINT32(MF_MT_SAMPLE_SIZE, format.width * format.height * 3u / 2u));
  return type.CopyTo(out);
}

HRESULT videoFormatFromMediaType(IMFMediaType* type, VideoFormat& out) {
  RC_RETURN_IF_NULL(type);

  GUID major = GUID_NULL;
  GUID subtype = GUID_NULL;
  RC_RETURN_IF_FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &major));
  RC_RETURN_IF_FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype));
  RC_RETURN_HR_IF(major != MFMediaType_Video || subtype != MFVideoFormat_NV12,
                  MF_E_INVALIDMEDIATYPE);

  VideoFormat parsed;
  RC_RETURN_IF_FAILED(
      ::MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &parsed.width, &parsed.height));
  RC_RETURN_IF_FAILED(::MFGetAttributeRatio(type, MF_MT_FRAME_RATE,
                                            &parsed.fpsNumerator,
                                            &parsed.fpsDenominator));
  RC_RETURN_HR_IF(!isSupportedVideoFormat(parsed), MF_E_INVALIDMEDIATYPE);
  out = parsed;
  return S_OK;
}

}  // namespace rcvcam
