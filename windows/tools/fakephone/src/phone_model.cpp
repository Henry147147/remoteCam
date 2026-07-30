#include "rcfakephone/phone_model.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace rcfakephone {
namespace {

rc::control::CameraDescriptor camera(std::string id, std::string name,
                                     std::string position, std::string lens,
                                     std::vector<rc::control::CaptureFormat> formats) {
  rc::control::CameraDescriptor result;
  result.id = std::move(id);
  result.name = std::move(name);
  result.position = std::move(position);
  result.lens = std::move(lens);
  result.formats = std::move(formats);
  return result;
}

}  // namespace

PhoneProfile standardProfile() {
  PhoneProfile profile;
  profile.capabilities.codecs = {"hevc", "h264"};
  profile.capabilities.cameras = {
      camera("back-ultra", "Back Ultra Wide", "back", "ultra-wide",
             {{1280, 720, 30}, {1920, 1080, 30}, {1920, 1080, 60}}),
      camera("back-wide", "Back Wide", "back", "wide",
             {{1280, 720, 30}, {1280, 720, 60}, {1920, 1080, 30}, {1920, 1080, 60}}),
      camera("back-tele", "Back Telephoto", "back", "tele",
             {{1280, 720, 30}, {1920, 1080, 30}}),
      camera("front-true-depth", "Front TrueDepth", "front", "true-depth",
             {{1280, 720, 30}, {1920, 1080, 30}}),
  };
  profile.camera.deviceId = "back-wide";
  profile.camera.position = "back";
  profile.camera.lens = "wide";
  profile.camera.zoom = 1.0;
  profile.camera.focusMode = "auto";
  profile.camera.focus = 0.5;
  profile.camera.exposureMode = "auto";
  profile.camera.iso = 100.0;
  profile.camera.exposureSeconds = 1.0 / 60.0;
  profile.camera.exposureBias = 0.0;
  profile.camera.whiteBalanceMode = "auto";
  profile.camera.whiteBalanceKelvin = 5000.0;
  profile.camera.torch = false;
  profile.camera.stabilization = true;
  return profile;
}

PhoneProfile constrainedProfile() {
  PhoneProfile profile = standardProfile();
  profile.deviceName = "RemoteCam Constrained iPhone";
  profile.model = "iPhone SE Emulator";
  profile.capabilities.codecs = {"h264"};
  profile.capabilities.cameras.resize(1);
  profile.capabilities.cameras[0].formats = {{1280, 720, 30}};
  profile.thermal.state = "serious";
  profile.battery = {0.12, false};
  return profile;
}

bool validDeviceId(const std::string& value) {
  return value.size() == 16 &&
         std::all_of(value.begin(), value.end(), [](unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

PhoneModel::PhoneModel(PhoneProfile profile) : profile_(std::move(profile)) {}

void PhoneModel::connected() { state_ = SessionState::Connected; }

void PhoneModel::helloSent() { state_ = SessionState::HelloSent; }

void PhoneModel::disconnected() { state_ = SessionState::Disconnected; }

bool PhoneModel::readConfig(const rc::control::Message& message,
                            rc::control::StreamConfig& out) const {
  std::string codec;
  uint64_t width = 0;
  uint64_t height = 0;
  uint64_t fps = 0;
  uint64_t bitrate = 0;
  if (!message.text("codec", codec) || !rc::control::codecFromName(codec, out.codec) ||
      !message.unsignedInt("width", width) || !message.unsignedInt("height", height) ||
      !message.unsignedInt("fps", fps) || !message.unsignedInt("bitrate", bitrate) ||
      width > UINT32_MAX || height > UINT32_MAX || fps > UINT32_MAX ||
      bitrate > UINT32_MAX) {
    return false;
  }
  out.width = static_cast<uint32_t>(width);
  out.height = static_cast<uint32_t>(height);
  out.fps = static_cast<uint32_t>(fps);
  out.bitrate = static_cast<uint32_t>(bitrate);
  return out.valid();
}

ControlEffects PhoneModel::apply(const rc::control::Message& message) {
  ControlEffects effects;
  if (message.type == "server_info") {
    bool paired = false;
    if (!message.boolean("paired", paired)) {
      effects.accepted = false;
      effects.reason = "server_info.paired is missing or not boolean";
      state_ = SessionState::Failed;
    } else if (!paired) {
      state_ = SessionState::AwaitingTrust;
    }
    return effects;
  }

  if (message.type == "pair_required") {
    state_ = SessionState::AwaitingTrust;
    return effects;
  }

  if (message.type == "ready" || message.type == "set_format") {
    rc::control::StreamConfig requested;
    if (!readConfig(message, requested)) {
      effects.accepted = false;
      effects.reason = "invalid stream configuration";
      state_ = SessionState::Failed;
      return effects;
    }
    const bool isReady = message.type == "ready";
    config_ = requested;
    effects.becameReady = isReady;
    effects.formatChanged = !isReady;
    state_ = isReady ? SessionState::Ready : state_;
    return effects;
  }

  if (message.type == "set_preview") {
    bool enabled = false;
    if (!message.boolean("enabled", enabled)) {
      effects.accepted = false;
      effects.reason = "set_preview.enabled is missing or not boolean";
      return effects;
    }
    previewEnabled_ = enabled;
    effects.previewChanged = true;
    return effects;
  }

  if (message.type == "set_camera") {
    std::string lens;
    if (!message.text("lens", lens)) {
      effects.accepted = false;
      effects.reason = "set_camera.lens is missing";
      return effects;
    }
    std::string position;
    const bool hasPosition = message.text("position", position);
    const auto match = std::find_if(
        profile_.capabilities.cameras.begin(), profile_.capabilities.cameras.end(),
        [&](const rc::control::CameraDescriptor& candidate) {
          return candidate.lens == lens && (!hasPosition || candidate.position == position);
        });
    if (match == profile_.capabilities.cameras.end()) {
      effects.accepted = false;
      effects.reason = "requested camera is not present in the profile";
      return effects;
    }
    profile_.camera.deviceId = match->id;
    profile_.camera.position = match->position;
    profile_.camera.lens = match->lens;
    effects.cameraChanged = true;
    effects.keyframeRequested = true;
    return effects;
  }

  if (message.type == "set_control") {
    double number = 0.0;
    bool boolean = false;
    std::string text;
    if (message.number("zoom", number)) profile_.camera.zoom = number;
    if (message.number("focus", number)) profile_.camera.focus = number;
    if (message.number("iso", number)) profile_.camera.iso = number;
    if (message.number("exposure", number)) profile_.camera.exposureSeconds = number;
    if (message.number("ev", number)) profile_.camera.exposureBias = number;
    if (message.number("wb", number)) profile_.camera.whiteBalanceKelvin = number;
    if (message.text("focus_mode", text)) profile_.camera.focusMode = text;
    if (message.text("exposure_mode", text)) profile_.camera.exposureMode = text;
    if (message.text("wb_mode", text)) profile_.camera.whiteBalanceMode = text;
    if (message.boolean("torch", boolean)) profile_.camera.torch = boolean;
    if (message.boolean("stabilization", boolean)) profile_.camera.stabilization = boolean;
    effects.controlsChanged = true;
    return effects;
  }

  if (message.type == "request_keyframe") {
    effects.keyframeRequested = true;
    return effects;
  }

  if (message.type == "stats") {
    uint64_t bitrate = 0;
    if (message.unsignedInt("target_bitrate", bitrate) && bitrate >= 64000 &&
        bitrate <= 100000000) {
      targetBitrate_ = bitrate;
      effects.statsReceived = true;
    }
    return effects;
  }

  // Unknown types are deliberately ignored, matching the protocol's additive
  // forward-compatibility rule.
  return effects;
}

std::vector<rc::control::Message> PhoneModel::startupMessages() const {
  return {
      rc::control::capabilities(profile_.capabilities),
      rc::control::cameraState(profile_.camera),
      rc::control::streamStart(),
      rc::control::orientation(profile_.orientation),
      rc::control::thermal(profile_.thermal),
      rc::control::battery(profile_.battery),
  };
}

}  // namespace rcfakephone
