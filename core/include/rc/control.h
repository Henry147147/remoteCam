// RemoteCam — control-channel messages.
//
// Channel 0 and channel 3 payloads: a single top-level CBOR map with a `t` key naming
// the message type. Normative source is docs/protocol.md "Control messages"; the
// shipping counterpart is ios/RemoteCam/Sources/Wire/ControlMessage.swift.
//
// FORWARD COMPATIBILITY IS THE POINT OF THIS LAYER
//
// Unknown keys are preserved by Message::decode so callers can diagnose them, but a
// known v1 parser rejects every field outside its exact schema. Unknown message types
// are an explicitly authenticated additive-extension policy at the session layer.
// Malformed control costs one message while a malformed *frame* costs the connection:
// framing loses the stream position, this does not.
//
// The 1 MiB cap here is separate from, and far below, the 16 MiB framing cap. Framing
// has to carry video; a control message that large is not a control message.
//
// Cryptography stays outside this portable message-shape layer. The normative profile
// is fixed in protocol.md; Windows implements it in rcwin-security and authenticates
// an envelope before passing the contained CBOR bytes to Message::decode.

#ifndef RC_CONTROL_H
#define RC_CONTROL_H

#include <cstdint>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "rc/cbor.h"

namespace rc::control {

// A control payload above this is refused before the CBOR decoder is even entered.
inline constexpr size_t kMaxPayloadBytes = size_t{1024} * 1024;

// The protocol version this implementation speaks. The v1 handshake requires an exact
// match rather than treating older or newer integers as compatible subsets.
inline constexpr uint64_t kProtocolVersion = 1;

enum class Error {
  None = 0,
  PayloadTooLarge,
  MalformedCbor,
  NotAMap,
  MissingType,
};

const char* errorText(Error error);

// The envelope. `fields` never contains `t`: it is stripped on decode and re-inserted
// on encode, which also means a field literally named `t` cannot survive a round trip.
// Swift behaves identically.
struct Message {
  std::string type;
  cbor::Map fields;

  std::vector<uint8_t> encode() const;

  // `cborError` receives the underlying CBOR failure when the result is MalformedCbor,
  // so a log can say which of the ten ways it was malformed.
  static Error decode(const uint8_t* data, size_t size, Message& out, cbor::Error& cborError);
  static Error decode(const std::vector<uint8_t>& data, Message& out, cbor::Error& cborError);

  // Convenience readers that apply the same strictness as the phone: a width sent as a
  // double is a protocol violation, not something to round.
  bool text(const char* key, std::string& out) const;
  bool bytes(const char* key, std::vector<uint8_t>& out) const;
  bool unsignedInt(const char* key, uint64_t& out) const;
  bool boolean(const char* key, bool& out) const;
  bool number(const char* key, double& out) const;
};

// ---------------------------------------------------------------------------
// Stream configuration

enum class Codec { H264, Hevc };

const char* codecName(Codec codec);
bool codecFromName(const std::string& name, Codec& out);

// Bounds copied from ios StreamConfiguration.validated(). They are enforced on this
// side because the phone hard-fails a session whose `ready` does not satisfy them --
// sending an invalid config is not a negotiation, it is a dropped connection.
struct StreamConfig {
  Codec codec = Codec::H264;
  uint32_t width = 1280;
  uint32_t height = 720;
  uint32_t fps = 30;
  uint32_t bitrate = 4000000;

  bool valid() const;
};

// protocol.md's "minimal v1" ordering: the PC opens with something conservative that
// any device can satisfy, the phone answers with `caps`, and the PC then upgrades with
// `set_format`. It cannot choose well before it knows what the device supports.
StreamConfig conservativeDefault();

// ---------------------------------------------------------------------------
// PC -> phone

Message serverInfo(const std::string& name, const std::string& id, bool paired,
                   const std::vector<std::string>& caps);
Message ready(const StreamConfig& config);
// The generation-bearing form is required for live reconfiguration. The legacy
// overload remains for protocol fixtures that only inspect the config shape; a
// SessionController never sends an unacknowledgeable set_format.
Message setFormat(const StreamConfig& config);
Message setFormat(const StreamConfig& config, uint64_t generation);
Message setCamera(const std::string& lens, const std::optional<std::string>& position);
Message requestKeyframe();
Message setPreview(bool enabled);

struct AuthChallenge {
  std::array<uint8_t, 32> serverNonce{};
  uint64_t expiresUnixSeconds = 0;
};
Message authChallenge(const AuthChallenge& value);

struct AuthConfirm {
  std::array<uint8_t, 32> serverProof{};
  uint64_t sessionExpiresUnixSeconds = 0;
};
Message authConfirm(const AuthConfirm& value);

// Every field optional: an absent key means "leave this alone" (protocol.md). Sending a
// default-valued key instead would silently overwrite whatever the user set on the
// phone.
struct CameraControls {
  std::optional<double> zoom;
  std::optional<double> focus;
  std::optional<double> iso;
  std::optional<double> exposure;
  std::optional<double> ev;
  std::optional<double> whiteBalance;
  std::optional<std::string> focusMode;
  std::optional<std::string> exposureMode;
  std::optional<std::string> whiteBalanceMode;
  std::optional<bool> torch;
  std::optional<bool> stabilization;
};
Message setControl(const CameraControls& controls);

// Channel 3, ~2 Hz. The phone retunes toward target_bitrate and never raises on its
// own, so this is the only thing that can bring the bitrate back up.
struct Stats {
  uint64_t queueDepth = 0;
  double decodeMillis = 0.0;
  uint64_t drops = 0;
  double rttMillis = 0.0;
  uint64_t targetBitrate = 0;
};
Message stats(const Stats& stats);

// ---------------------------------------------------------------------------
// phone -> PC

// Builders mirror ios/RemoteCam/Sources/Wire/ControlMessage.swift. Keeping them in
// the shared protocol library makes the Windows phone emulator byte-for-byte
// compatible with the shipping client instead of maintaining a second collection of
// ad-hoc CBOR maps in the test tool.
Message hello(const std::string& deviceName, const std::string& deviceId,
              const std::string& model, const std::vector<std::string>& caps,
              const std::string& platform = "ios");
Message streamStart();

struct AuthResponse {
  std::array<uint8_t, 32> clientNonce{};
  std::array<uint8_t, 32> clientProof{};
};
Message authResponse(const AuthResponse& value);
bool parseAuthResponse(const Message& message, AuthResponse& out);
Message formatAck(uint64_t generation);
bool parseFormatAck(const Message& message, uint64_t& generation);

// A transactional live-format rebuild may fail on the phone after old-generation
// media has stopped. The rejection rolls the PC back to its last committed format;
// generation still identifies the exact pending request and zero remains reserved.
struct FormatReject {
  uint64_t generation = 0;
  std::string code;
  std::string message;
};
Message formatReject(const FormatReject& value);
bool parseFormatReject(const Message& message, FormatReject& out);

// Stable host/device identities in v1 are exactly 64 bits rendered as canonical
// lowercase hexadecimal. Accepting aliases would let one pairing record be addressed
// by multiple attacker-controlled strings.
bool validDeviceId(std::string_view value);

struct Hello {
  uint64_t version = 0;
  std::string deviceName;
  std::string deviceId;
  std::string platform;
  std::string model;
  std::vector<std::string> caps;

  bool supports(const std::string& capability) const;
};
bool parseHello(const Message& message, Hello& out);

struct Orientation {
  double degrees = 0.0;
  bool locked = false;
};
Message orientation(const Orientation& value);
bool parseOrientation(const Message& message, Orientation& out);

struct Thermal {
  std::string state;   // nominal | fair | serious | critical
};
Message thermal(const Thermal& value);
bool parseThermal(const Message& message, Thermal& out);

struct Battery {
  double level = 0.0;
  bool charging = false;
};
Message battery(const Battery& value);
bool parseBattery(const Message& message, Battery& out);

struct CameraState {
  std::optional<std::string> deviceId;
  std::string position;
  std::string lens;
  double zoom = 0.0;
  std::string focusMode;
  double focus = 0.0;
  std::string exposureMode;
  double iso = 0.0;
  double exposureSeconds = 0.0;
  double exposureBias = 0.0;
  std::string whiteBalanceMode;
  double whiteBalanceKelvin = 0.0;
  bool torch = false;
  bool stabilization = false;
};
Message cameraState(const CameraState& value);
bool parseCameraState(const Message& message, CameraState& out);

struct DeviceError {
  std::string code;
  std::string message;
};
Message deviceError(const DeviceError& value);
bool parseError(const Message& message, DeviceError& out);

struct CaptureFormat {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t fps = 0;
};

struct CameraDescriptor {
  std::string id;
  std::string name;
  std::string position;   // front | back
  std::string lens;       // ultra-wide | wide | tele | true-depth | other
  std::vector<CaptureFormat> formats;
};

struct Caps {
  std::vector<CameraDescriptor> cameras;
  std::vector<std::string> codecs;

  // Best codec both ends understand, preferring HEVC per protocol.md.
  bool preferredCodec(Codec& out) const;
};
Message capabilities(const Caps& value);
bool parseCaps(const Message& message, Caps& out);

}  // namespace rc::control

#endif  // RC_CONTROL_H
