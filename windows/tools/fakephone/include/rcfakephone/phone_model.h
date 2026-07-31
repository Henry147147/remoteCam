// Stateful model of the parts of an iPhone that are observable on the wire.

#ifndef RCFAKEPHONE_PHONE_MODEL_H
#define RCFAKEPHONE_PHONE_MODEL_H

#include <cstdint>
#include <string>
#include <vector>

#include "rc/control.h"

namespace rcfakephone {

enum class SessionState {
  Disconnected,
  Connected,
  HelloSent,
  AwaitingTrust,
  Ready,
  Streaming,
  Failed,
};

struct PhoneProfile {
  std::string deviceName = "RemoteCam Emulated iPhone";
  std::string deviceId = "72636d756c61746f";
  std::string model = "iPhone Emulator";
  rc::control::Caps capabilities;
  rc::control::CameraState camera;
  rc::control::Orientation orientation{0.0, false};
  rc::control::Thermal thermal{"nominal"};
  rc::control::Battery battery{0.82, true};
};

PhoneProfile standardProfile();
PhoneProfile constrainedProfile();
bool validDeviceId(const std::string& value);

struct ControlEffects {
  bool accepted = true;
  bool becameReady = false;
  bool formatChanged = false;
  uint64_t formatGeneration = 0;
  bool cameraChanged = false;
  bool controlsChanged = false;
  bool previewChanged = false;
  bool keyframeRequested = false;
  bool statsReceived = false;
  std::string reason;
};

class PhoneModel {
 public:
  explicit PhoneModel(PhoneProfile profile = standardProfile());

  void connected();
  void helloSent();
  void disconnected();

  // Applies one PC -> phone message using the same validation/default rules as the
  // Swift app. Unknown messages are accepted and ignored for forward compatibility.
  ControlEffects apply(const rc::control::Message& message);

  std::vector<rc::control::Message> startupMessages() const;

  const PhoneProfile& profile() const { return profile_; }
  const rc::control::StreamConfig& streamConfig() const { return config_; }
  SessionState state() const { return state_; }
  bool previewEnabled() const { return previewEnabled_; }
  uint64_t targetBitrate() const { return targetBitrate_; }

 private:
  bool readConfig(const rc::control::Message& message, rc::control::StreamConfig& out) const;

  PhoneProfile profile_;
  rc::control::StreamConfig config_ = rc::control::conservativeDefault();
  SessionState state_ = SessionState::Disconnected;
  bool previewEnabled_ = true;
  uint64_t targetBitrate_ = 0;
  uint64_t lastFormatGeneration_ = 0;
};

}  // namespace rcfakephone

#endif  // RCFAKEPHONE_PHONE_MODEL_H
