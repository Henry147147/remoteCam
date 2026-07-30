#include "rcplatform/video_decoder.h"

#include <climits>
#include <cstring>
#include <utility>

#ifdef RC_WITH_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixfmt.h>
}
#endif

namespace rcplatform {

#ifdef RC_WITH_FFMPEG
namespace {

class FfmpegD3D11Decoder final : public IVideoDecoder {
 public:
  explicit FfmpegD3D11Decoder(rc::annexb::Codec codec) : codec_(codec) {}

  ~FfmpegD3D11Decoder() override {
    if (frame_ != nullptr) av_frame_free(&frame_);
    if (context_ != nullptr) avcodec_free_context(&context_);
    if (device_ != nullptr) av_buffer_unref(&device_);
  }

  HRESULT initialize() {
    const AVCodecID codecId =
        codec_ == rc::annexb::Codec::H264 ? AV_CODEC_ID_H264 : AV_CODEC_ID_HEVC;
    const AVCodec* codec = avcodec_find_decoder(codecId);
    if (codec == nullptr) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

    bool supportsD3D11 = false;
    for (int index = 0;; ++index) {
      const AVCodecHWConfig* config = avcodec_get_hw_config(codec, index);
      if (config == nullptr) break;
      if ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0 &&
          config->device_type == AV_HWDEVICE_TYPE_D3D11VA &&
          config->pix_fmt == AV_PIX_FMT_D3D11) {
        supportsD3D11 = true;
        break;
      }
    }
    if (!supportsD3D11) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);

    int error = av_hwdevice_ctx_create(&device_, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0);
    if (error < 0) return E_FAIL;

    context_ = avcodec_alloc_context3(codec);
    if (context_ == nullptr) return E_OUTOFMEMORY;
    context_->opaque = this;
    context_->get_format = &FfmpegD3D11Decoder::selectPixelFormat;
    context_->hw_device_ctx = av_buffer_ref(device_);
    if (context_->hw_device_ctx == nullptr) return E_OUTOFMEMORY;
    context_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    context_->pkt_timebase = AVRational{1, 1000000};

    error = avcodec_open2(context_, codec, nullptr);
    if (error < 0) return E_FAIL;
    frame_ = av_frame_alloc();
    return frame_ != nullptr ? S_OK : E_OUTOFMEMORY;
  }

  HRESULT decode(const uint8_t* annexB, size_t size, uint64_t ptsMicros,
                 TextureFrame& out) override {
    if (context_ == nullptr || frame_ == nullptr) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
    if ((annexB == nullptr && size != 0) || size > static_cast<size_t>(INT_MAX) ||
        ptsMicros > static_cast<uint64_t>(INT64_MAX)) {
      return E_INVALIDARG;
    }

    AVPacket* packet = av_packet_alloc();
    if (packet == nullptr) return E_OUTOFMEMORY;
    int error = av_new_packet(packet, static_cast<int>(size));
    if (error >= 0 && size != 0) std::memcpy(packet->data, annexB, size);
    if (error >= 0) {
      packet->pts = static_cast<int64_t>(ptsMicros);
      packet->dts = static_cast<int64_t>(ptsMicros);
      error = avcodec_send_packet(context_, packet);
    }
    av_packet_free(&packet);
    if (error < 0) return E_FAIL;

    av_frame_unref(frame_);
    error = avcodec_receive_frame(context_, frame_);
    if (error == AVERROR(EAGAIN) || error == AVERROR_EOF) return S_FALSE;
    if (error < 0) return E_FAIL;
    if (frame_->format != AV_PIX_FMT_D3D11 || frame_->data[0] == nullptr) return E_UNEXPECTED;

    auto* texture = reinterpret_cast<ID3D11Texture2D*>(frame_->data[0]);
    out.texture = texture;
    out.arraySlice = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(frame_->data[1]));
    out.width = static_cast<uint32_t>(frame_->width);
    out.height = static_cast<uint32_t>(frame_->height);
    out.ptsMicros = ptsMicros;
    return S_OK;
  }

  void flush() override {
    if (context_ != nullptr) avcodec_flush_buffers(context_);
  }

 private:
  static AVPixelFormat selectPixelFormat(AVCodecContext* context,
                                         const AVPixelFormat* formats) {
    auto* self = static_cast<FfmpegD3D11Decoder*>(context->opaque);
    if (self == nullptr || formats == nullptr) return AV_PIX_FMT_NONE;
    for (const AVPixelFormat* candidate = formats; *candidate != AV_PIX_FMT_NONE;
         ++candidate) {
      if (*candidate == AV_PIX_FMT_D3D11) {
        return SUCCEEDED(self->createShaderReadablePool(context)) ? *candidate
                                                                  : AV_PIX_FMT_NONE;
      }
    }
    return AV_PIX_FMT_NONE;
  }

  HRESULT createShaderReadablePool(AVCodecContext* context) {
    AVBufferRef* frames = nullptr;
    int error = avcodec_get_hw_frames_parameters(context, device_, AV_PIX_FMT_D3D11, &frames);
    if (error < 0 || frames == nullptr) return E_FAIL;

    auto* frameContext = reinterpret_cast<AVHWFramesContext*>(frames->data);
    auto* d3d11Context = reinterpret_cast<AVD3D11VAFramesContext*>(frameContext->hwctx);
    d3d11Context->BindFlags |= D3D11_BIND_SHADER_RESOURCE;
    error = av_hwframe_ctx_init(frames);
    if (error < 0) {
      av_buffer_unref(&frames);
      return E_FAIL;
    }
    av_buffer_unref(&context->hw_frames_ctx);
    context->hw_frames_ctx = frames;
    return S_OK;
  }

  rc::annexb::Codec codec_;
  AVBufferRef* device_ = nullptr;
  AVCodecContext* context_ = nullptr;
  AVFrame* frame_ = nullptr;
};

}  // namespace
#endif

HRESULT createFfmpegD3D11Decoder(rc::annexb::Codec codec,
                                 std::unique_ptr<IVideoDecoder>& out) {
  out.reset();
#ifdef RC_WITH_FFMPEG
  auto decoder = std::make_unique<FfmpegD3D11Decoder>(codec);
  const HRESULT hr = decoder->initialize();
  if (FAILED(hr)) return hr;
  out = std::move(decoder);
  return S_OK;
#else
  static_cast<void>(codec);
  return E_NOTIMPL;
#endif
}

}  // namespace rcplatform
