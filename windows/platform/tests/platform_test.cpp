#include "rcplatform/abr_controller.h"
#include "rcplatform/pixel_convert.h"
#include "rcplatform/shader_constants.h"
#include "rcplatform/video_pipeline.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

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

void checkNear(float got, float want, float tolerance, const std::string& what) {
  check(std::fabs(got - want) <= tolerance,
        what + " (got " + std::to_string(got) + ", want " + std::to_string(want) + ")");
}

void appendNal(std::vector<uint8_t>& bytes, uint8_t type) {
  bytes.insert(bytes.end(), {0, 0, 0, 1, static_cast<uint8_t>(0x60u | type), 0xaa});
}

void testAbrBackoffAndRecovery() {
  std::printf("ABR backs off fast and recovers slowly\n");
  rcplatform::AbrController controller(4'000'000, 500'000, 8'000'000);

  check(controller.observe({1, 0, 10'000'000}) == 4'000'000,
        "the first queue sample establishes a baseline");
  check(controller.observe({2, 0, 10'000'000}) == 4'000'000,
        "one growing sample does not react to jitter");
  check(controller.observe({3, 0, 10'000'000}) == 3'200'000,
        "sustained growth cuts twenty percent");
  check(controller.observe({3, 1, 10'000'000}) == 2'560'000,
        "a newly dropped frame cuts immediately");

  for (int sample = 0; sample < 9; ++sample) {
    check(controller.observe({0, 1, 10'000'000}) == 2'560'000,
          "recovery waits for five stable seconds");
  }
  check(controller.observe({0, 1, 10'000'000}) == 2'688'000,
        "recovery is a five-percent step");

  // Sweep repeated congestion and prove the target cannot walk below its floor.
  uint64_t drops = 1;
  for (int sample = 0; sample < 100; ++sample) {
    ++drops;
    controller.observe({10, drops, 1'000'000});
  }
  check(controller.targetBitsPerSec() == 500'000, "repeated backoff stops at the minimum");

  // A throughput estimate below the configured floor must not invert std::clamp's
  // bounds or lower the target beneath a decodable bitrate.
  for (int sample = 0; sample < 10; ++sample) controller.observe({0, drops, 100'000});
  check(controller.targetBitsPerSec() == 500'000,
        "recovery ceiling below the floor leaves the floor intact");
}

void testShaderPackingIsExact() {
  std::printf("Shader constants preserve rc::destToSource exactly\n");

  rc::TransformParams params;
  params.srcWidth = 1280;
  params.srcHeight = 720;
  params.dstWidth = 1920;
  params.dstHeight = 1080;
  params.rotationDeg = 37.0f;
  params.flipH = true;
  params.fit = rc::FitMode::Fill;
  params.zoom = 1.2f;
  params.panX = 23.0f;
  params.panY = -11.0f;

  const rc::Mat3 expected = rc::destToSource(params);
  const rcplatform::TransformConstants packed = rcplatform::packTransformConstants(params);
  const float* rows[] = {packed.row0, packed.row1, packed.row2};
  for (int row = 0; row < 3; ++row) {
    for (int column = 0; column < 3; ++column) {
      checkNear(rows[row][column], expected.m[row * 3 + column], 0.0f,
                "matrix element reaches the cbuffer without transposition");
    }
    checkNear(rows[row][3], 0.0f, 0.0f, "row padding is deterministic");
  }

  for (rc::Vec2 destination :
       {rc::Vec2{0, 0}, rc::Vec2{960, 540}, rc::Vec2{1919, 1079}, rc::Vec2{313, 777}}) {
    const rc::Vec2 direct = expected.apply(destination);
    const float homogeneous = packed.row2[0] * destination.x +
                              packed.row2[1] * destination.y + packed.row2[2];
    const rc::Vec2 shader{
        (packed.row0[0] * destination.x + packed.row0[1] * destination.y +
         packed.row0[2]) /
            homogeneous,
        (packed.row1[0] * destination.x + packed.row1[1] * destination.y +
         packed.row1[2]) /
            homogeneous};
    checkNear(shader.x, direct.x, 1e-5f, "shader x matches the core transform");
    checkNear(shader.y, direct.y, 1e-5f, "shader y matches the core transform");
  }
  checkNear(packed.inverseSourceSize[0], 1.0f / 1280.0f, 0.0f, "inverse source width");
  checkNear(packed.inverseSourceSize[1], 1.0f / 720.0f, 0.0f, "inverse source height");
  checkNear(packed.inverseSourceSize[2], 1280.0f, 0.0f, "source width for matte bounds");
  checkNear(packed.inverseSourceSize[3], 720.0f, 0.0f, "source height for matte bounds");
}

void testBgraToNv12() {
  std::printf("BGRA to studio-range Rec.709 NV12\n");
  constexpr uint32_t width = 4;
  constexpr uint32_t height = 2;
  std::vector<uint8_t> bgra(width * height * 4u, 0);
  std::vector<uint8_t> nv12(width * height * 3u / 2u, 0xff);

  check(rcplatform::bgraToNv12(bgra.data(), bgra.size(), width * 4u, width, height,
                               nv12.data(), nv12.size(), width) == S_OK,
        "black frame converts");
  for (uint32_t index = 0; index < width * height; ++index) {
    check(nv12[index] == 16, "black luma is video-range black");
  }
  for (size_t index = width * height; index < nv12.size(); ++index) {
    check(nv12[index] == 128, "neutral black chroma is centred");
  }

  for (size_t index = 0; index < bgra.size(); index += 4u) {
    bgra[index + 0u] = 255;
    bgra[index + 1u] = 255;
    bgra[index + 2u] = 255;
    bgra[index + 3u] = 255;
  }
  check(rcplatform::bgraToNv12(bgra.data(), bgra.size(), width * 4u, width, height,
                               nv12.data(), nv12.size(), width) == S_OK,
        "white frame converts");
  check(nv12[0] == 235, "white luma is video-range white");
  check(nv12[width * height] == 128 && nv12[width * height + 1u] == 128,
        "white chroma remains neutral");

  check(rcplatform::bgraToNv12(bgra.data(), bgra.size(), width * 4u, 3, height,
                               nv12.data(), nv12.size(), width) == E_INVALIDARG,
        "odd output width is rejected");
  check(rcplatform::bgraToNv12(bgra.data(), bgra.size() - 1u, width * 4u, width, height,
                               nv12.data(), nv12.size(), width) ==
            HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER),
        "short mapped input is rejected");
}

class RecordingDecoder final : public rcplatform::IVideoDecoder {
 public:
  HRESULT decode(const uint8_t*, size_t, uint64_t, rcplatform::TextureFrame& out) override {
    events->push_back("decode");
    ++calls;
    if (result != S_OK) return result;
    out.width = 1280;
    out.height = 720;
    out.ptsMicros = 999;  // pipeline must replace this with the wire timestamp
    return S_OK;
  }
  void flush() override {}

  std::vector<std::string>* events = nullptr;
  HRESULT result = S_OK;
  int calls = 0;
};

class RecordingTransform final : public rcplatform::IFrameTransform {
 public:
  HRESULT apply(const rcplatform::TextureFrame& input, const rc::TransformParams&,
                rcplatform::TextureFrame& out) override {
    events->push_back("transform");
    if (result != S_OK) return result;
    out = input;
    out.width = 1920;
    out.height = 1080;
    return S_OK;
  }
  std::vector<std::string>* events = nullptr;
  HRESULT result = S_OK;
};

class RecordingEffects final : public rcplatform::IEffectChain {
 public:
  HRESULT apply(rcplatform::TextureFrame&) override {
    events->push_back("effects");
    return result;
  }
  std::vector<std::string>* events = nullptr;
  HRESULT result = S_OK;
};

class RecordingSink final : public rcplatform::IFrameSink {
 public:
  HRESULT publish(const rcplatform::TextureFrame& frame) override {
    events->push_back("sink");
    pts = frame.ptsMicros;
    width = frame.width;
    return result;
  }
  std::vector<std::string>* events = nullptr;
  HRESULT result = S_OK;
  uint64_t pts = 0;
  uint32_t width = 0;
};

void testPipelineValidationOrderAndPts() {
  std::printf("Pipeline validation, ordering and PTS propagation\n");

  std::vector<std::string> events;
  RecordingDecoder decoder;
  RecordingTransform transform;
  RecordingEffects effects;
  RecordingSink sink;
  decoder.events = transform.events = effects.events = sink.events = &events;
  rcplatform::VideoPipeline pipeline(decoder, transform, &effects, sink);

  std::vector<uint8_t> keyframe;
  appendNal(keyframe, 7);  // SPS
  appendNal(keyframe, 8);  // PPS
  appendNal(keyframe, 5);  // IDR
  rcplatform::EncodedAccessUnit unit{keyframe.data(), keyframe.size(),
                                     rc::annexb::Codec::H264, true, 1'234'567};
  rc::TransformParams params;
  const rcplatform::PipelineOutcome outcome = pipeline.push(unit, params);
  check(outcome.result == rcplatform::PipelineResult::Published, "a valid unit publishes");
  check(events == std::vector<std::string>({"decode", "transform", "effects", "sink"}),
        "transform runs before effects and the sink is last");
  check(sink.pts == unit.ptsMicros, "the wire PTS reaches the sink unchanged");
  check(sink.width == 1920, "the sink receives the transformed frame");

  events.clear();
  std::vector<uint8_t> missing;
  appendNal(missing, 5);
  unit.bytes = missing.data();
  unit.size = missing.size();
  check(pipeline.push(unit, params).result ==
            rcplatform::PipelineResult::KeyframeMissingParameterSets,
        "a flagged keyframe without SPS/PPS is rejected");
  check(events.empty(), "bad keyframe data never reaches the decoder");

  const std::vector<uint8_t> avcc = {0, 0, 0, 2, 0x65, 0xaa};
  unit.bytes = avcc.data();
  unit.size = avcc.size();
  check(pipeline.push(unit, params).result == rcplatform::PipelineResult::InvalidAnnexB,
        "length-prefixed video is rejected at the seam");

  unit.bytes = keyframe.data();
  unit.size = keyframe.size();
  decoder.result = S_FALSE;
  check(pipeline.push(unit, params).result == rcplatform::PipelineResult::DecoderNeedsMoreInput,
        "decoder reordering is not treated as a failure");
}

}  // namespace

int main() {
  testAbrBackoffAndRecovery();
  testShaderPackingIsExact();
  testBgraToNv12();
  testPipelineValidationOrderAndPts();
  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
