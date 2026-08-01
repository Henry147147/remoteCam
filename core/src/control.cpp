#include "rc/control.h"

#include <algorithm>
#include <initializer_list>
#include <utility>

namespace rc::control {
namespace {

constexpr const char* kTypeKey = "t";

void put(cbor::Map& map, const char* key, cbor::Value value) {
  map.insert_or_assign(std::string(key), std::move(value));
}

Message makeMessage(const char* type) {
  Message message;
  message.type = type;
  return message;
}

void putStreamConfig(cbor::Map& fields, const StreamConfig& config) {
  put(fields, "codec", cbor::Value::text(codecName(config.codec)));
  put(fields, "width", cbor::Value::unsignedInt(config.width));
  put(fields, "height", cbor::Value::unsignedInt(config.height));
  put(fields, "fps", cbor::Value::unsignedInt(config.fps));
  put(fields, "bitrate", cbor::Value::unsignedInt(config.bitrate));
}

bool textAt(const cbor::Map& map, const char* key, std::string& out) {
  const auto it = map.find(std::string(key));
  if (it == map.end()) return false;
  const std::string* value = nullptr;
  if (!it->second.asText(value)) return false;
  out = *value;
  return true;
}

bool unsignedAt(const cbor::Map& map, const char* key, uint64_t& out) {
  const auto it = map.find(std::string(key));
  if (it == map.end()) return false;
  return it->second.asUnsigned(out);
}

bool onlyFields(const cbor::Map& map,
                std::initializer_list<std::string_view> allowed) {
  return std::all_of(map.begin(), map.end(), [&](const auto& entry) {
    return std::find(allowed.begin(), allowed.end(),
                     std::string_view(entry.first)) != allowed.end();
  });
}

}  // namespace

const char* errorText(Error error) {
  switch (error) {
    case Error::None: return "none";
    case Error::PayloadTooLarge: return "control payload too large";
    case Error::MalformedCbor: return "malformed cbor";
    case Error::NotAMap: return "payload is not a map";
    case Error::MissingType: return "missing or non-text type key";
  }
  return "unknown";
}

std::vector<uint8_t> Message::encode() const {
  cbor::Map map = fields;
  // Inserted last, so it wins over any caller-supplied "t" exactly as Swift's
  // `map["t"] = .string(type)` does.
  map.insert_or_assign(std::string(kTypeKey), cbor::Value::text(type));
  return cbor::encode(cbor::Value::map(std::move(map)));
}

Error Message::decode(const uint8_t* data, size_t size, Message& out, cbor::Error& cborError) {
  cborError = cbor::Error::None;
  // Checked before decoding, not after: the cap exists to bound the work, so applying
  // it afterwards would defeat it.
  if (size > kMaxPayloadBytes) return Error::PayloadTooLarge;

  cbor::Value value;
  cborError = cbor::decode(data, size, value);
  if (cborError != cbor::Error::None) return Error::MalformedCbor;

  const cbor::Map* map = nullptr;
  if (!value.asMap(map)) return Error::NotAMap;

  const auto typeIt = map->find(std::string(kTypeKey));
  if (typeIt == map->end()) return Error::MissingType;
  const std::string* type = nullptr;
  if (!typeIt->second.asText(type)) return Error::MissingType;

  out.type = *type;
  out.fields = *map;
  out.fields.erase(std::string(kTypeKey));
  return Error::None;
}

Error Message::decode(const std::vector<uint8_t>& data, Message& out, cbor::Error& cborError) {
  return decode(data.data(), data.size(), out, cborError);
}

bool Message::text(const char* key, std::string& out) const { return textAt(fields, key, out); }

bool Message::bytes(const char* key, std::vector<uint8_t>& out) const {
  const auto it = fields.find(std::string(key));
  if (it == fields.end()) return false;
  const std::vector<uint8_t>* value = nullptr;
  if (!it->second.asBytes(value)) return false;
  out = *value;
  return true;
}

bool Message::unsignedInt(const char* key, uint64_t& out) const {
  return unsignedAt(fields, key, out);
}

bool Message::boolean(const char* key, bool& out) const {
  const auto it = fields.find(std::string(key));
  if (it == fields.end()) return false;
  return it->second.asBoolean(out);
}

bool Message::number(const char* key, double& out) const {
  const auto it = fields.find(std::string(key));
  if (it == fields.end()) return false;
  return it->second.numericDouble(out);
}

const char* codecName(Codec codec) { return codec == Codec::Hevc ? "hevc" : "h264"; }

bool codecFromName(const std::string& name, Codec& out) {
  if (name == "hevc") {
    out = Codec::Hevc;
    return true;
  }
  if (name == "h264") {
    out = Codec::H264;
    return true;
  }
  return false;
}

bool StreamConfig::valid() const {
  // Mirrors ios StreamConfiguration.validated(). Even dimensions are not cosmetic:
  // NV12 chroma is subsampled 2x2, so an odd dimension has no well-defined chroma
  // plane.
  if (width == 0 || height == 0) return false;
  if (width % 2 != 0 || height % 2 != 0) return false;
  if (width > 4096 || height > 4096) return false;
  if (static_cast<uint64_t>(width) * height > 3840ull * 2160ull) return false;
  if (fps < 1 || fps > 120) return false;
  if (bitrate < 64000 || bitrate > 100000000) return false;
  return true;
}

StreamConfig conservativeDefault() {
  StreamConfig config;
  config.codec = Codec::H264;
  config.width = 1280;
  config.height = 720;
  config.fps = 30;
  config.bitrate = 4000000;
  return config;
}

Message serverInfo(const std::string& name, const std::string& id, bool paired,
                   bool allowUnauthenticated, const std::vector<std::string>& caps) {
  Message message = makeMessage("server_info");
  put(message.fields, "v", cbor::Value::unsignedInt(kProtocolVersion));
  put(message.fields, "name", cbor::Value::text(name));
  put(message.fields, "id", cbor::Value::text(id));
  put(message.fields, "paired", cbor::Value::boolean(paired));
  put(message.fields, "allow_unauthenticated", cbor::Value::boolean(allowUnauthenticated));
  cbor::Array list;
  list.reserve(caps.size());
  for (const std::string& cap : caps) list.push_back(cbor::Value::text(cap));
  put(message.fields, "caps", cbor::Value::array(std::move(list)));
  return message;
}

Message ready(const StreamConfig& config) {
  Message message = makeMessage("ready");
  putStreamConfig(message.fields, config);
  return message;
}

Message setFormat(const StreamConfig& config) {
  Message message = makeMessage("set_format");
  putStreamConfig(message.fields, config);
  return message;
}

Message setFormat(const StreamConfig& config, uint64_t generation) {
  Message message = setFormat(config);
  put(message.fields, "generation", cbor::Value::unsignedInt(generation));
  return message;
}

Message setCamera(const std::string& lens, const std::optional<std::string>& position) {
  Message message = makeMessage("set_camera");
  put(message.fields, "lens", cbor::Value::text(lens));
  // Omitted rather than sent empty: the phone reads an absent position as "any", and an
  // empty string would match no camera at all.
  if (position.has_value()) put(message.fields, "position", cbor::Value::text(*position));
  return message;
}

Message requestKeyframe() { return makeMessage("request_keyframe"); }

Message setPreview(bool enabled) {
  Message message = makeMessage("set_preview");
  put(message.fields, "enabled", cbor::Value::boolean(enabled));
  return message;
}

Message authChallenge(const AuthChallenge& value) {
  Message message = makeMessage("auth_challenge");
  put(message.fields, "server_nonce",
      cbor::Value::bytes(std::vector<uint8_t>(value.serverNonce.begin(),
                                               value.serverNonce.end())));
  put(message.fields, "expires",
      cbor::Value::unsignedInt(value.expiresUnixSeconds));
  return message;
}

Message authConfirm(const AuthConfirm& value) {
  Message message = makeMessage("auth_confirm");
  put(message.fields, "server_proof",
      cbor::Value::bytes(std::vector<uint8_t>(value.serverProof.begin(),
                                               value.serverProof.end())));
  put(message.fields, "session_expires",
      cbor::Value::unsignedInt(value.sessionExpiresUnixSeconds));
  return message;
}

Message setControl(const CameraControls& controls) {
  Message message = makeMessage("set_control");
  const auto putNumber = [&](const char* key, const std::optional<double>& value) {
    if (value.has_value()) put(message.fields, key, cbor::Value::real(*value));
  };
  const auto putText = [&](const char* key, const std::optional<std::string>& value) {
    if (value.has_value()) put(message.fields, key, cbor::Value::text(*value));
  };
  const auto putBool = [&](const char* key, const std::optional<bool>& value) {
    if (value.has_value()) put(message.fields, key, cbor::Value::boolean(*value));
  };

  putNumber("zoom", controls.zoom);
  putNumber("focus", controls.focus);
  putNumber("iso", controls.iso);
  putNumber("exposure", controls.exposure);
  putNumber("ev", controls.ev);
  putNumber("wb", controls.whiteBalance);
  putText("focus_mode", controls.focusMode);
  putText("exposure_mode", controls.exposureMode);
  putText("wb_mode", controls.whiteBalanceMode);
  putBool("torch", controls.torch);
  putBool("stabilization", controls.stabilization);
  return message;
}

Message stats(const Stats& value) {
  Message message = makeMessage("stats");
  put(message.fields, "queue_depth", cbor::Value::unsignedInt(value.queueDepth));
  put(message.fields, "decode_ms", cbor::Value::real(value.decodeMillis));
  put(message.fields, "drops", cbor::Value::unsignedInt(value.drops));
  put(message.fields, "rtt_ms", cbor::Value::real(value.rttMillis));
  put(message.fields, "target_bitrate", cbor::Value::unsignedInt(value.targetBitrate));
  return message;
}

bool Hello::supports(const std::string& capability) const {
  return std::find(caps.begin(), caps.end(), capability) != caps.end();
}

Message hello(const std::string& deviceName, const std::string& deviceId,
              const std::string& model, const std::vector<std::string>& caps,
              const std::string& platform, bool allowUnauthenticated) {
  Message message = makeMessage("hello");
  put(message.fields, "v", cbor::Value::unsignedInt(kProtocolVersion));
  put(message.fields, "device_name", cbor::Value::text(deviceName));
  put(message.fields, "device_id", cbor::Value::text(deviceId));
  put(message.fields, "platform", cbor::Value::text(platform));
  put(message.fields, "model", cbor::Value::text(model));
  put(message.fields, "allow_unauthenticated", cbor::Value::boolean(allowUnauthenticated));
  cbor::Array list;
  list.reserve(caps.size());
  for (const std::string& cap : caps) list.push_back(cbor::Value::text(cap));
  put(message.fields, "caps", cbor::Value::array(std::move(list)));
  return message;
}

Message streamStart() { return makeMessage("stream_start"); }

Message authResponse(const AuthResponse& value) {
  Message message = makeMessage("auth_response");
  put(message.fields, "client_nonce",
      cbor::Value::bytes(std::vector<uint8_t>(value.clientNonce.begin(),
                                               value.clientNonce.end())));
  put(message.fields, "client_proof",
      cbor::Value::bytes(std::vector<uint8_t>(value.clientProof.begin(),
                                               value.clientProof.end())));
  return message;
}

bool parseAuthResponse(const Message& message, AuthResponse& out) {
  if (message.type != "auth_response" ||
      !onlyFields(message.fields, {"client_nonce", "client_proof"})) {
    return false;
  }
  std::vector<uint8_t> nonce;
  std::vector<uint8_t> proof;
  if (!message.bytes("client_nonce", nonce) || nonce.size() != out.clientNonce.size() ||
      !message.bytes("client_proof", proof) || proof.size() != out.clientProof.size()) {
    return false;
  }
  std::copy(nonce.begin(), nonce.end(), out.clientNonce.begin());
  std::copy(proof.begin(), proof.end(), out.clientProof.begin());
  return true;
}

Message formatAck(uint64_t generation) {
  Message message = makeMessage("format_ack");
  put(message.fields, "generation", cbor::Value::unsignedInt(generation));
  return message;
}

bool parseFormatAck(const Message& message, uint64_t& generation) {
  return message.type == "format_ack" && onlyFields(message.fields, {"generation"}) &&
         message.unsignedInt("generation", generation) &&
         generation != 0;
}

Message formatReject(const FormatReject& value) {
  Message message = makeMessage("format_reject");
  put(message.fields, "generation", cbor::Value::unsignedInt(value.generation));
  put(message.fields, "code", cbor::Value::text(value.code));
  put(message.fields, "message", cbor::Value::text(value.message));
  return message;
}

bool parseFormatReject(const Message& message, FormatReject& out) {
  if (message.type != "format_reject" ||
      !onlyFields(message.fields, {"generation", "code", "message"}) ||
      !message.unsignedInt("generation", out.generation) || out.generation == 0 ||
      !message.text("code", out.code) || out.code.empty() || out.code.size() > 128 ||
      !message.text("message", out.message) || out.message.empty() ||
      out.message.size() > 1024) {
    return false;
  }
  return true;
}

bool validDeviceId(std::string_view value) {
  if (value.size() != 16) return false;
  return std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

bool parseHello(const Message& message, Hello& out) {
  if (message.type != "hello" ||
      !onlyFields(message.fields, {"v", "device_name", "device_id", "platform", "model",
                                   "caps", "allow_unauthenticated"})) {
    return false;
  }
  if (!message.unsignedInt("v", out.version) ||
      !message.text("device_id", out.deviceId) || !validDeviceId(out.deviceId) ||
      !message.text("device_name", out.deviceName) || out.deviceName.empty() ||
      !message.text("platform", out.platform) || out.platform.empty() ||
      !message.text("model", out.model) || out.model.empty()) {
    return false;
  }
  // Tolerated-absent: a client built before the pairing opt-out existed omits this and
  // must still parse. Defaulting to false is the safe direction -- a missing field can
  // only ever withhold the downgrade, never request it.
  if (!message.boolean("allow_unauthenticated", out.allowUnauthenticated)) {
    out.allowUnauthenticated = false;
  }

  out.caps.clear();
  const auto capsIt = message.fields.find(std::string("caps"));
  if (capsIt == message.fields.end()) return false;
  const cbor::Array* list = nullptr;
  if (!capsIt->second.asArray(list)) return false;
  for (const cbor::Value& item : *list) {
    const std::string* text = nullptr;
    if (!item.asText(text)) return false;
    out.caps.push_back(*text);
  }
  return true;
}

bool parseOrientation(const Message& message, Orientation& out) {
  if (message.type != "orientation" ||
      !onlyFields(message.fields, {"deg", "locked"})) return false;
  if (!message.number("deg", out.degrees)) return false;
  if (!message.boolean("locked", out.locked)) out.locked = false;
  return true;
}

Message orientation(const Orientation& value) {
  Message message = makeMessage("orientation");
  put(message.fields, "deg", cbor::Value::real(value.degrees));
  put(message.fields, "locked", cbor::Value::boolean(value.locked));
  return message;
}

bool parseThermal(const Message& message, Thermal& out) {
  if (message.type != "thermal" || !onlyFields(message.fields, {"state"})) return false;
  return message.text("state", out.state);
}

Message thermal(const Thermal& value) {
  Message message = makeMessage("thermal");
  put(message.fields, "state", cbor::Value::text(value.state));
  return message;
}

bool parseBattery(const Message& message, Battery& out) {
  if (message.type != "battery" ||
      !onlyFields(message.fields, {"level", "charging"})) return false;
  if (!message.number("level", out.level)) return false;
  if (!message.boolean("charging", out.charging)) out.charging = false;
  return true;
}

Message battery(const Battery& value) {
  Message message = makeMessage("battery");
  put(message.fields, "level", cbor::Value::real(value.level));
  put(message.fields, "charging", cbor::Value::boolean(value.charging));
  return message;
}

bool parseCameraState(const Message& message, CameraState& out) {
  if (message.type != "camera_state" ||
      !onlyFields(message.fields,
                  {"device_id", "position", "lens", "zoom", "focus_mode", "focus",
                   "exposure_mode", "iso", "exposure", "ev", "wb_mode", "wb",
                   "torch", "stabilization"})) {
    return false;
  }

  const auto deviceIt = message.fields.find(std::string("device_id"));
  if (deviceIt != message.fields.end()) {
    if (deviceIt->second.isNull()) {
      out.deviceId.reset();
    } else {
      const std::string* id = nullptr;
      if (!deviceIt->second.asText(id)) return false;
      out.deviceId = *id;
    }
  } else {
    out.deviceId.reset();
  }

  if (!message.text("position", out.position) || !message.text("lens", out.lens) ||
      !message.number("zoom", out.zoom) || !message.text("focus_mode", out.focusMode) ||
      !message.number("focus", out.focus) ||
      !message.text("exposure_mode", out.exposureMode) || !message.number("iso", out.iso) ||
      !message.number("exposure", out.exposureSeconds) ||
      !message.number("ev", out.exposureBias) ||
      !message.text("wb_mode", out.whiteBalanceMode) ||
      !message.number("wb", out.whiteBalanceKelvin) || !message.boolean("torch", out.torch)) {
    return false;
  }
  // Added by the shipping Swift client after the original protocol table. Old clients
  // omit it, which means disabled rather than making the whole state unusable.
  if (!message.boolean("stabilization", out.stabilization)) out.stabilization = false;
  return true;
}

Message cameraState(const CameraState& value) {
  Message message = makeMessage("camera_state");
  put(message.fields, "device_id",
      value.deviceId.has_value() ? cbor::Value::text(*value.deviceId) : cbor::Value::null());
  put(message.fields, "position", cbor::Value::text(value.position));
  put(message.fields, "lens", cbor::Value::text(value.lens));
  put(message.fields, "zoom", cbor::Value::real(value.zoom));
  put(message.fields, "focus_mode", cbor::Value::text(value.focusMode));
  put(message.fields, "focus", cbor::Value::real(value.focus));
  put(message.fields, "exposure_mode", cbor::Value::text(value.exposureMode));
  put(message.fields, "iso", cbor::Value::real(value.iso));
  put(message.fields, "exposure", cbor::Value::real(value.exposureSeconds));
  put(message.fields, "ev", cbor::Value::real(value.exposureBias));
  put(message.fields, "wb_mode", cbor::Value::text(value.whiteBalanceMode));
  put(message.fields, "wb", cbor::Value::real(value.whiteBalanceKelvin));
  put(message.fields, "torch", cbor::Value::boolean(value.torch));
  put(message.fields, "stabilization", cbor::Value::boolean(value.stabilization));
  return message;
}

bool parseError(const Message& message, DeviceError& out) {
  if (message.type != "error" ||
      !onlyFields(message.fields, {"code", "message"})) return false;
  return message.text("code", out.code) && message.text("message", out.message);
}

Message deviceError(const DeviceError& value) {
  Message message = makeMessage("error");
  put(message.fields, "code", cbor::Value::text(value.code));
  put(message.fields, "message", cbor::Value::text(value.message));
  return message;
}

bool Caps::preferredCodec(Codec& out) const {
  // HEVC preferred, H.264 fallback (protocol.md "Video"). Order in the phone's list is
  // not treated as preference -- hello and caps disagree about it on iOS today.
  bool hasH264 = false;
  for (const std::string& name : codecs) {
    if (name == "hevc") {
      out = Codec::Hevc;
      return true;
    }
    if (name == "h264") hasH264 = true;
  }
  if (hasH264) {
    out = Codec::H264;
    return true;
  }
  return false;
}

bool parseCaps(const Message& message, Caps& out) {
  if (message.type != "caps" ||
      !onlyFields(message.fields, {"cameras", "codecs"})) return false;

  out.cameras.clear();
  out.codecs.clear();

  const auto camerasIt = message.fields.find(std::string("cameras"));
  if (camerasIt == message.fields.end()) return false;
  const cbor::Array* cameras = nullptr;
  if (!camerasIt->second.asArray(cameras)) return false;
  for (const cbor::Value& entry : *cameras) {
        const cbor::Map* camera = nullptr;
        if (!entry.asMap(camera) ||
            !onlyFields(*camera, {"id", "name", "position", "lens", "formats"})) {
          return false;
        }

        CameraDescriptor descriptor;
        // A camera without an id cannot be selected later, so it is not worth keeping.
        if (!textAt(*camera, "id", descriptor.id) || descriptor.id.empty() ||
            !textAt(*camera, "name", descriptor.name) ||
            !textAt(*camera, "position", descriptor.position) ||
            !textAt(*camera, "lens", descriptor.lens)) return false;

        const auto formatsIt = camera->find(std::string("formats"));
        if (formatsIt == camera->end()) return false;
        const cbor::Array* formats = nullptr;
        if (!formatsIt->second.asArray(formats)) return false;
            for (const cbor::Value& formatEntry : *formats) {
              const cbor::Map* format = nullptr;
              if (!formatEntry.asMap(format) ||
                  !onlyFields(*format, {"width", "height", "fps"})) return false;
              uint64_t width = 0, height = 0, fps = 0;
              if (!unsignedAt(*format, "width", width) ||
                  !unsignedAt(*format, "height", height) ||
                  !unsignedAt(*format, "fps", fps)) return false;
              // Anything that cannot fit the ring is not a format we can accept, and
              // silently keeping it would surface later as a dropped frame.
              if (width == 0 || height == 0 || fps == 0 ||
                  width > 4096 || height > 4096 || fps > 120) return false;
              descriptor.formats.push_back(CaptureFormat{static_cast<uint32_t>(width),
                                                         static_cast<uint32_t>(height),
                                                         static_cast<uint32_t>(fps)});
            }
        out.cameras.push_back(std::move(descriptor));
  }

  const auto codecsIt = message.fields.find(std::string("codecs"));
  if (codecsIt == message.fields.end()) return false;
  const cbor::Array* codecs = nullptr;
  if (!codecsIt->second.asArray(codecs)) return false;
      for (const cbor::Value& item : *codecs) {
        const std::string* text = nullptr;
        if (!item.asText(text) || (*text != "h264" && *text != "hevc")) return false;
        out.codecs.push_back(*text);
      }
  return true;
}

Message capabilities(const Caps& value) {
  Message message = makeMessage("caps");
  cbor::Array cameras;
  cameras.reserve(value.cameras.size());
  for (const CameraDescriptor& descriptor : value.cameras) {
    cbor::Map camera;
    put(camera, "id", cbor::Value::text(descriptor.id));
    put(camera, "name", cbor::Value::text(descriptor.name));
    put(camera, "position", cbor::Value::text(descriptor.position));
    put(camera, "lens", cbor::Value::text(descriptor.lens));

    std::vector<CaptureFormat> formats = descriptor.formats;
    std::sort(formats.begin(), formats.end(), [](const CaptureFormat& lhs,
                                                  const CaptureFormat& rhs) {
      if (lhs.width != rhs.width) return lhs.width < rhs.width;
      if (lhs.height != rhs.height) return lhs.height < rhs.height;
      return lhs.fps < rhs.fps;
    });
    cbor::Array encodedFormats;
    encodedFormats.reserve(formats.size());
    for (const CaptureFormat& format : formats) {
      cbor::Map encoded;
      put(encoded, "width", cbor::Value::unsignedInt(format.width));
      put(encoded, "height", cbor::Value::unsignedInt(format.height));
      put(encoded, "fps", cbor::Value::unsignedInt(format.fps));
      encodedFormats.push_back(cbor::Value::map(std::move(encoded)));
    }
    put(camera, "formats", cbor::Value::array(std::move(encodedFormats)));
    cameras.push_back(cbor::Value::map(std::move(camera)));
  }
  put(message.fields, "cameras", cbor::Value::array(std::move(cameras)));

  cbor::Array codecs;
  codecs.reserve(value.codecs.size());
  for (const std::string& codec : value.codecs) {
    codecs.push_back(cbor::Value::text(codec));
  }
  put(message.fields, "codecs", cbor::Value::array(std::move(codecs)));
  return message;
}

}  // namespace rc::control
