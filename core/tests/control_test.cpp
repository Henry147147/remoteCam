// Tests for the control-message layer.
//
// Two properties carry most of the weight. First, the field names and value *types*
// must match what the phone reads -- ios reads width/height/fps/bitrate strictly as
// CBOR unsigned, so sending them as doubles produces a session the phone hard-fails
// with "invalid video configuration". Second, the forward-compatibility rules: unknown
// keys survive and unknown types are not errors, because that pair is what lets a newer
// phone talk to an older PC.

#include "rc/control.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("  FAIL: %s\n", what.c_str());
  }
}

using rc::control::Message;
using rc::cbor::Value;

Message roundTrip(const Message& original, bool& ok) {
  Message decoded;
  rc::cbor::Error cborError = rc::cbor::Error::None;
  const rc::control::Error err =
      Message::decode(original.encode(), decoded, cborError);
  ok = err == rc::control::Error::None;
  if (!ok) {
    std::printf("  (decode failed: %s / %s)\n", rc::control::errorText(err),
                rc::cbor::errorText(cborError));
  }
  return decoded;
}

// ---------------------------------------------------------------------------

void testEnvelope() {
  std::printf("Envelope\n");

  Message message;
  message.type = "ready";
  message.fields.insert_or_assign("codec", Value::text("hevc"));

  // The envelope is a plain CBOR map with "t" folded in, so it must produce exactly the
  // bytes the CBOR tests already pinned against the Swift suite.
  const std::vector<uint8_t> expected = {0xa2, 0x61, 0x74, 0x65, 0x72, 0x65, 0x61, 0x64, 0x79,
                                         0x65, 0x63, 0x6f, 0x64, 0x65, 0x63, 0x64, 0x68, 0x65,
                                         0x76, 0x63};
  check(message.encode() == expected, "the envelope encodes as the shared byte vector");

  bool ok = false;
  const Message decoded = roundTrip(message, ok);
  check(ok, "the envelope decodes");
  check(decoded.type == "ready", "the type is lifted out of the map");
  check(decoded.fields.find("t") == decoded.fields.end(),
        "the type key is stripped from the fields");
  check(decoded.fields.size() == 1, "the remaining fields survive");

  // A caller-supplied "t" loses to the real type, matching Swift's encoded().
  Message shadowed;
  shadowed.type = "ready";
  shadowed.fields.insert_or_assign("t", Value::text("impostor"));
  Message back = roundTrip(shadowed, ok);
  check(ok && back.type == "ready", "a field named t cannot shadow the message type");
}

void testEnvelopeRejections() {
  std::printf("Envelope rejections\n");

  Message decoded;
  rc::cbor::Error cborError = rc::cbor::Error::None;

  // Not a map: an array payload is well-formed CBOR and still not a control message.
  const std::vector<uint8_t> array = {0x81, 0x01};
  check(Message::decode(array, decoded, cborError) == rc::control::Error::NotAMap,
        "a non-map payload is refused");

  // A map with no "t".
  const std::vector<uint8_t> noType = {0xa1, 0x61, 0x78, 0x01};
  check(Message::decode(noType, decoded, cborError) == rc::control::Error::MissingType,
        "a map without a type is refused");

  // "t" present but not a string.
  const std::vector<uint8_t> numericType = {0xa1, 0x61, 0x74, 0x01};
  check(Message::decode(numericType, decoded, cborError) == rc::control::Error::MissingType,
        "a non-text type is refused");

  // Malformed CBOR surfaces the underlying reason so a log can be specific.
  const std::vector<uint8_t> truncated = {0xa1, 0x61};
  check(Message::decode(truncated, decoded, cborError) == rc::control::Error::MalformedCbor,
        "malformed cbor is refused");
  check(cborError == rc::cbor::Error::Truncated, "and the cbor reason is reported");

  // The cap is applied before decoding, so an oversized payload costs nothing to reject.
  const std::vector<uint8_t> huge(rc::control::kMaxPayloadBytes + 1, 0);
  check(Message::decode(huge, decoded, cborError) == rc::control::Error::PayloadTooLarge,
        "an oversized control payload is refused before decoding");
}

void testForwardCompatibility() {
  std::printf("Forward compatibility\n");

  // A newer phone sends fields this build has never heard of. They must survive rather
  // than cause a rejection -- this is the rule that keeps old PCs working.
  Message future;
  future.type = "camera_state";
  future.fields.insert_or_assign("zoom", Value::real(2.0));
  future.fields.insert_or_assign("some_future_key", Value::text("value"));
  future.fields.insert_or_assign("another", Value::array({Value::unsignedInt(1)}));

  bool ok = false;
  const Message decoded = roundTrip(future, ok);
  check(ok, "a message with unknown keys decodes");
  check(decoded.fields.find("some_future_key") != decoded.fields.end(),
        "unknown keys are preserved, not dropped");
  check(decoded.fields.size() == 3, "every field survives");

  // An unknown message type is a valid envelope. Deciding to ignore it belongs to the
  // session layer; refusing it here would close connections on additive changes.
  Message unknown;
  unknown.type = "invented_in_2027";
  unknown.fields.insert_or_assign("x", Value::unsignedInt(1));
  const Message back = roundTrip(unknown, ok);
  check(ok && back.type == "invented_in_2027", "an unknown message type decodes cleanly");
}

void testStreamConfigValidation() {
  std::printf("Stream configuration bounds\n");

  check(rc::control::conservativeDefault().valid(), "the conservative default is valid");

  // Bounds copied from the phone. If these drift the symptom is a session that the
  // phone kills as soon as `ready` arrives, with the cause on the other machine.
  const struct {
    rc::control::StreamConfig config;
    bool valid;
    const char* what;
  } cases[] = {
      {{rc::control::Codec::H264, 1920, 1080, 30, 4000000}, true, "1080p30"},
      {{rc::control::Codec::Hevc, 3840, 2160, 60, 40000000}, true, "4K60 at the pixel cap"},
      {{rc::control::Codec::H264, 1919, 1080, 30, 4000000}, false, "odd width"},
      {{rc::control::Codec::H264, 1920, 1081, 30, 4000000}, false, "odd height"},
      {{rc::control::Codec::H264, 4098, 1080, 30, 4000000}, false, "width over 4096"},
      {{rc::control::Codec::H264, 4096, 4096, 30, 4000000}, false, "over the pixel budget"},
      {{rc::control::Codec::H264, 1920, 1080, 0, 4000000}, false, "zero fps"},
      {{rc::control::Codec::H264, 1920, 1080, 121, 4000000}, false, "fps over 120"},
      {{rc::control::Codec::H264, 1920, 1080, 30, 63999}, false, "bitrate below the floor"},
      {{rc::control::Codec::H264, 1920, 1080, 30, 100000001}, false, "bitrate above the ceiling"},
      {{rc::control::Codec::H264, 0, 1080, 30, 4000000}, false, "zero width"},
  };
  for (const auto& c : cases) {
    check(c.config.valid() == c.valid, std::string(c.what) + " validates correctly");
  }
}

void testPcToPhoneMessages() {
  std::printf("PC -> phone messages\n");

  // The phone reads these five strictly as CBOR unsigned. Encoding them as doubles is
  // the single most likely way to produce a session that dies on connect, so assert the
  // type rather than just the value.
  const rc::control::StreamConfig config = rc::control::conservativeDefault();
  bool ok = false;
  Message readyBack = roundTrip(rc::control::ready(config), ok);
  check(ok && readyBack.type == "ready", "ready round-trips");
  for (const char* key : {"width", "height", "fps", "bitrate"}) {
    const auto it = readyBack.fields.find(std::string(key));
    check(it != readyBack.fields.end() && it->second.type() == Value::Type::Unsigned,
          std::string("ready.") + key + " is a CBOR unsigned, not a double");
  }
  std::string codec;
  check(readyBack.text("codec", codec) && codec == "h264", "ready carries the codec name");

  Message formatBack = roundTrip(rc::control::setFormat(config), ok);
  check(ok && formatBack.type == "set_format", "set_format round-trips");

  // Absent position means "any camera in that lens class"; sending an empty string
  // would match nothing.
  Message camera = roundTrip(rc::control::setCamera("wide", std::nullopt), ok);
  check(ok && camera.fields.find("position") == camera.fields.end(),
        "an absent position is omitted rather than sent empty");
  Message camera2 = roundTrip(rc::control::setCamera("tele", std::string("back")), ok);
  std::string position;
  check(ok && camera2.text("position", position) && position == "back",
        "a present position is sent");

  Message keyframe = roundTrip(rc::control::requestKeyframe(), ok);
  check(ok && keyframe.type == "request_keyframe" && keyframe.fields.empty(),
        "request_keyframe carries no fields");

  Message preview = roundTrip(rc::control::setPreview(false), ok);
  bool enabled = true;
  check(ok && preview.boolean("enabled", enabled) && !enabled, "set_preview carries a bool");

  Message info = roundTrip(rc::control::serverInfo("Desk PC", "0123456789abcdef", false,
                                                   {"h264", "hevc"}), ok);
  bool paired = true;
  check(ok && info.boolean("paired", paired) && !paired, "server_info reports pairing state");
  uint64_t version = 0;
  check(info.unsignedInt("v", version) && version == rc::control::kProtocolVersion,
        "server_info announces the protocol version");
}

void testSetControlIsSparse() {
  std::printf("set_control omits what it does not set\n");

  // "absent keys mean leave alone" (protocol.md). A builder that emitted defaults would
  // silently reset every manual camera setting on the phone each time one changed.
  rc::control::CameraControls controls;
  controls.zoom = 2.5;
  controls.torch = true;

  bool ok = false;
  const Message message = roundTrip(rc::control::setControl(controls), ok);
  check(ok, "set_control round-trips");
  check(message.fields.size() == 2, "only the fields that were set are present");

  double zoom = 0.0;
  check(message.number("zoom", zoom) && zoom == 2.5, "zoom is carried");
  bool torch = false;
  check(message.boolean("torch", torch) && torch, "torch is carried");
  for (const char* absent : {"iso", "exposure", "ev", "wb", "focus", "focus_mode",
                             "exposure_mode", "wb_mode", "stabilization"}) {
    check(message.fields.find(std::string(absent)) == message.fields.end(),
          std::string("set_control omits ") + absent);
  }

  const Message empty = roundTrip(rc::control::setControl(rc::control::CameraControls{}), ok);
  check(ok && empty.fields.empty(), "an empty control update carries no fields at all");
}

void testPhoneToPcParsing() {
  std::printf("phone -> PC parsing\n");

  // Built the way the phone builds it, per ControlMessage.swift.
  Message hello;
  hello.type = "hello";
  hello.fields.insert_or_assign("v", Value::unsignedInt(1));
  hello.fields.insert_or_assign("device_name", Value::text("Test iPhone"));
  hello.fields.insert_or_assign("device_id", Value::text("0123456789abcdef"));
  hello.fields.insert_or_assign("platform", Value::text("ios"));
  hello.fields.insert_or_assign("model", Value::text("iPhone"));
  hello.fields.insert_or_assign("caps", Value::array({Value::text("h264"), Value::text("hevc")}));

  bool ok = false;
  const Message helloBack = roundTrip(hello, ok);
  rc::control::Hello parsed;
  check(ok && rc::control::parseHello(helloBack, parsed), "hello parses");
  check(parsed.version == 1 && parsed.deviceId == "0123456789abcdef", "hello identity fields");
  check(parsed.supports("hevc") && !parsed.supports("av1"), "capability lookup");

  // Wrong type, and missing mandatory fields, both refuse rather than half-fill.
  Message wrongType = helloBack;
  wrongType.type = "caps";
  check(!rc::control::parseHello(wrongType, parsed), "parseHello rejects another type");
  Message noId = helloBack;
  noId.fields.erase("device_id");
  check(!rc::control::parseHello(noId, parsed), "hello without a device id is refused");

  Message orientation;
  orientation.type = "orientation";
  orientation.fields.insert_or_assign("deg", Value::real(90.0));
  orientation.fields.insert_or_assign("locked", Value::boolean(false));
  rc::control::Orientation orientationOut;
  check(rc::control::parseOrientation(roundTrip(orientation, ok), orientationOut) &&
            orientationOut.degrees == 90.0,
        "orientation parses");

  Message battery;
  battery.type = "battery";
  battery.fields.insert_or_assign("level", Value::real(0.42));
  battery.fields.insert_or_assign("charging", Value::boolean(true));
  rc::control::Battery batteryOut;
  check(rc::control::parseBattery(roundTrip(battery, ok), batteryOut) &&
            batteryOut.charging && batteryOut.level == 0.42,
        "battery parses");

  Message thermal;
  thermal.type = "thermal";
  thermal.fields.insert_or_assign("state", Value::text("serious"));
  rc::control::Thermal thermalOut;
  check(rc::control::parseThermal(roundTrip(thermal, ok), thermalOut) &&
            thermalOut.state == "serious",
        "thermal parses");

  Message cameraState;
  cameraState.type = "camera_state";
  cameraState.fields.insert_or_assign("device_id", Value::null());
  cameraState.fields.insert_or_assign("position", Value::text("back"));
  cameraState.fields.insert_or_assign("lens", Value::text("wide"));
  cameraState.fields.insert_or_assign("zoom", Value::real(1.5));
  cameraState.fields.insert_or_assign("focus_mode", Value::text("manual"));
  cameraState.fields.insert_or_assign("focus", Value::real(0.25));
  cameraState.fields.insert_or_assign("exposure_mode", Value::text("locked"));
  cameraState.fields.insert_or_assign("iso", Value::real(125.0));
  cameraState.fields.insert_or_assign("exposure", Value::real(1.0 / 60.0));
  cameraState.fields.insert_or_assign("ev", Value::real(-0.5));
  cameraState.fields.insert_or_assign("wb_mode", Value::text("manual"));
  cameraState.fields.insert_or_assign("wb", Value::real(5600.0));
  cameraState.fields.insert_or_assign("torch", Value::boolean(true));
  cameraState.fields.insert_or_assign("stabilization", Value::boolean(true));
  rc::control::CameraState cameraStateOut;
  check(rc::control::parseCameraState(roundTrip(cameraState, ok), cameraStateOut),
        "camera_state parses all documented fields");
  check(!cameraStateOut.deviceId.has_value() && cameraStateOut.lens == "wide" &&
            cameraStateOut.zoom == 1.5 && cameraStateOut.iso == 125.0 &&
            cameraStateOut.torch && cameraStateOut.stabilization,
        "camera_state preserves null identity, units and booleans");

  Message deviceError;
  deviceError.type = "error";
  deviceError.fields.insert_or_assign("code", Value::text("capture_failed"));
  deviceError.fields.insert_or_assign("message", Value::text("camera unavailable"));
  rc::control::DeviceError errorOut;
  check(rc::control::parseError(roundTrip(deviceError, ok), errorOut) &&
            errorOut.code == "capture_failed",
        "device error parses");
  deviceError.fields.erase("message");
  check(!rc::control::parseError(deviceError, errorOut),
        "device error missing its message is rejected");
}

void testPhoneToPcBuilders() {
  std::printf("phone -> PC builders\n");

  bool ok = false;
  const Message hello = roundTrip(
      rc::control::hello("Emulated iPhone", "0123456789abcdef", "iPhone", {"h264", "hevc"}),
      ok);
  rc::control::Hello parsedHello;
  check(ok && rc::control::parseHello(hello, parsedHello), "hello builder parses");
  check(parsedHello.deviceName == "Emulated iPhone" && parsedHello.platform == "ios" &&
            parsedHello.supports("hevc"),
        "hello builder matches the iOS identity shape");

  check(roundTrip(rc::control::streamStart(), ok).type == "stream_start",
        "stream_start builder has the expected type");

  rc::control::Orientation orientation{37.0, false};
  rc::control::Orientation orientationOut;
  check(rc::control::parseOrientation(roundTrip(rc::control::orientation(orientation), ok),
                                      orientationOut) &&
            orientationOut.degrees == 37.0 && !orientationOut.locked,
        "orientation builder round-trips");

  rc::control::Thermal thermal{"serious"};
  rc::control::Thermal thermalOut;
  check(rc::control::parseThermal(roundTrip(rc::control::thermal(thermal), ok), thermalOut) &&
            thermalOut.state == "serious",
        "thermal builder round-trips");

  rc::control::Battery battery{0.64, true};
  rc::control::Battery batteryOut;
  check(rc::control::parseBattery(roundTrip(rc::control::battery(battery), ok), batteryOut) &&
            batteryOut.level == 0.64 && batteryOut.charging,
        "battery builder round-trips");

  rc::control::CameraState state;
  state.deviceId = "back-wide";
  state.position = "back";
  state.lens = "wide";
  state.zoom = 2.0;
  state.focusMode = "manual";
  state.focus = 0.33;
  state.exposureMode = "locked";
  state.iso = 160.0;
  state.exposureSeconds = 1.0 / 120.0;
  state.exposureBias = -0.25;
  state.whiteBalanceMode = "manual";
  state.whiteBalanceKelvin = 5200.0;
  state.torch = true;
  state.stabilization = true;
  rc::control::CameraState stateOut;
  check(rc::control::parseCameraState(roundTrip(rc::control::cameraState(state), ok), stateOut),
        "camera_state builder parses");
  check(stateOut.deviceId == state.deviceId && stateOut.zoom == 2.0 && stateOut.torch &&
            stateOut.stabilization,
        "camera_state builder preserves every state family");

  rc::control::DeviceError deviceError{"capture_failed", "simulated camera loss"};
  rc::control::DeviceError errorOut;
  check(rc::control::parseError(roundTrip(rc::control::deviceError(deviceError), ok), errorOut) &&
            errorOut.code == deviceError.code && errorOut.message == deviceError.message,
        "error builder round-trips");
}

void testCapsParsing() {
  std::printf("caps parsing\n");

  Value format1 = Value::map([] {
    rc::cbor::Map m;
    m.insert_or_assign("width", Value::unsignedInt(1920));
    m.insert_or_assign("height", Value::unsignedInt(1080));
    m.insert_or_assign("fps", Value::unsignedInt(60));
    return m;
  }());
  // Beyond what the ring can carry: kept out rather than accepted and dropped later.
  Value tooBig = Value::map([] {
    rc::cbor::Map m;
    m.insert_or_assign("width", Value::unsignedInt(8192));
    m.insert_or_assign("height", Value::unsignedInt(4320));
    m.insert_or_assign("fps", Value::unsignedInt(30));
    return m;
  }());
  Value camera = Value::map([&] {
    rc::cbor::Map m;
    m.insert_or_assign("id", Value::text("camera-1"));
    m.insert_or_assign("name", Value::text("Back Wide"));
    m.insert_or_assign("position", Value::text("back"));
    m.insert_or_assign("lens", Value::text("wide"));
    m.insert_or_assign("formats", Value::array({format1, tooBig}));
    return m;
  }());
  // No id: unselectable, so it is discarded rather than listed.
  Value anonymous = Value::map([] {
    rc::cbor::Map m;
    m.insert_or_assign("name", Value::text("Mystery"));
    return m;
  }());

  Message caps;
  caps.type = "caps";
  caps.fields.insert_or_assign("cameras", Value::array({camera, anonymous}));
  caps.fields.insert_or_assign("codecs", Value::array({Value::text("hevc"), Value::text("h264")}));

  bool ok = false;
  rc::control::Caps parsed;
  check(rc::control::parseCaps(roundTrip(caps, ok), parsed), "caps parses");
  check(parsed.cameras.size() == 1, "a camera without an id is discarded");
  if (!parsed.cameras.empty()) {
    check(parsed.cameras[0].id == "camera-1" && parsed.cameras[0].lens == "wide",
          "camera descriptor fields");
    check(parsed.cameras[0].formats.size() == 1, "an unusable format is discarded");
    if (!parsed.cameras[0].formats.empty()) {
      check(parsed.cameras[0].formats[0].width == 1920 &&
                parsed.cameras[0].formats[0].fps == 60,
            "format fields survive");
    }
  }

  rc::control::Codec codec = rc::control::Codec::H264;
  check(parsed.preferredCodec(codec) && codec == rc::control::Codec::Hevc,
        "hevc is preferred when offered");

  rc::control::Caps h264Only;
  h264Only.codecs = {"h264"};
  check(h264Only.preferredCodec(codec) && codec == rc::control::Codec::H264,
        "h264 is the fallback");
  rc::control::Caps none;
  none.codecs = {"av1"};
  check(!none.preferredCodec(codec), "an unknown-only codec list yields nothing");
}

void testCapsBuilder() {
  std::printf("caps builder\n");

  rc::control::CameraDescriptor back;
  back.id = "back-wide";
  back.name = "Back Wide";
  back.position = "back";
  back.lens = "wide";
  // Deliberately unsorted: Swift sorts these before encoding and the emulator must do
  // the same so capture fixtures are deterministic.
  back.formats = {{1920, 1080, 60}, {1280, 720, 30}};
  rc::control::Caps caps;
  caps.cameras = {back};
  caps.codecs = {"hevc", "h264"};

  bool ok = false;
  const Message encoded = roundTrip(rc::control::capabilities(caps), ok);
  rc::control::Caps decoded;
  check(ok && rc::control::parseCaps(encoded, decoded), "caps builder parses");
  check(decoded.cameras.size() == 1 && decoded.codecs == caps.codecs,
        "caps builder preserves cameras and codec order");
  if (!decoded.cameras.empty()) {
    check(decoded.cameras[0].formats.size() == 2 &&
              decoded.cameras[0].formats[0].width == 1280 &&
              decoded.cameras[0].formats[1].fps == 60,
          "caps builder orders formats like the iOS app");
  }
}

void testStats() {
  std::printf("stats\n");

  rc::control::Stats value;
  value.queueDepth = 3;
  value.decodeMillis = 7.5;
  value.drops = 2;
  value.rttMillis = 12.25;
  value.targetBitrate = 6000000;

  bool ok = false;
  const Message message = roundTrip(rc::control::stats(value), ok);
  check(ok && message.type == "stats", "stats round-trips");
  // The phone reads target_bitrate strictly as unsigned and clamps it to
  // 64,000...100,000,000; a double here would be dropped and the bitrate never recover.
  const auto it = message.fields.find(std::string("target_bitrate"));
  check(it != message.fields.end() && it->second.type() == Value::Type::Unsigned,
        "target_bitrate is a CBOR unsigned");
  uint64_t target = 0;
  check(message.unsignedInt("target_bitrate", target) && target == 6000000,
        "target_bitrate survives");
}

}  // namespace

int main() {
  testEnvelope();
  testEnvelopeRejections();
  testForwardCompatibility();
  testStreamConfigValidation();
  testPcToPhoneMessages();
  testSetControlIsSparse();
  testPhoneToPcParsing();
  testPhoneToPcBuilders();
  testCapsParsing();
  testCapsBuilder();
  testStats();

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
