// rc-fakepc.exe -- walking-skeleton backend for a Debug iOS build.
//
// This executable is intentionally not the production receiver. Pairing cannot be
// implemented until docs/ios-backend-handoff.md's crypto decisions are normative, so
// the tool refuses to send `ready` unless --allow-insecure-session is present. It is
// never installed. A Release iOS build ignores the corresponding launch argument and
// still rejects the session.

#include <windows.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "rc/annexb.h"
#include "rc/control.h"
#include "rcnet/bonjour_service.h"
#include "rcnet/tcp_listener.h"
#include "rcwin/hr.h"

#if !defined(RC_FAKEPC_INSECURE_ENABLED)

int wmain(int, wchar_t**) {
  std::fwprintf(stderr,
                L"rc-fakepc's insecure session path is compiled out of Release builds.\n"
                L"Build the Debug configuration for local integration.\n");
  return 2;
}

#else

namespace {

std::atomic<bool> g_stop{false};

BOOL WINAPI consoleHandler(DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
    g_stop.store(true, std::memory_order_release);
    return TRUE;
  }
  return FALSE;
}

struct Options {
  bool allowInsecure = false;
  bool advertise = true;
  uint16_t port = rcnet::kDefaultPort;
  std::wstring outputPath = L"remotecam-annexb.bin";
};

int usage() {
  std::fwprintf(
      stderr,
      L"Usage: rc-fakepc --allow-insecure-session [--port N] [--output PATH] "
      L"[--no-bonjour]\n\n"
      L"Launch the iOS Debug app with --allow-insecure-session too. This bypass is "
      L"for local integration only and is not present in RemoteCam.exe.\n");
  return 2;
}

bool parsePort(const wchar_t* value, uint16_t& out) {
  if (value == nullptr || *value == L'\0') return false;
  wchar_t* end = nullptr;
  errno = 0;
  const unsigned long parsed = std::wcstoul(value, &end, 10);
  if (errno != 0 || end == value || *end != L'\0' || parsed == 0 || parsed > 65535) {
    return false;
  }
  out = static_cast<uint16_t>(parsed);
  return true;
}

bool parseOptions(int argc, wchar_t** argv, Options& out) {
  for (int i = 1; i < argc; ++i) {
    const std::wstring argument(argv[i]);
    if (argument == L"--allow-insecure-session") {
      out.allowInsecure = true;
    } else if (argument == L"--no-bonjour") {
      out.advertise = false;
    } else if (argument == L"--port" && i + 1 < argc) {
      if (!parsePort(argv[++i], out.port)) return false;
    } else if (argument == L"--output" && i + 1 < argc) {
      out.outputPath = argv[++i];
    } else {
      return false;
    }
  }
  return out.allowInsecure;
}

std::string utf8(const std::wstring& value) {
  if (value.empty()) return {};
  const int count = ::WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0,
                                          nullptr, nullptr);
  if (count <= 0) return {};
  std::string result(static_cast<size_t>(count), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), count, nullptr, nullptr);
  return result;
}

HRESULT sendControl(rcnet::Connection& connection, const rc::control::Message& message,
                    uint8_t channel = static_cast<uint8_t>(rc::wire::Channel::Control)) {
  const std::vector<uint8_t> payload = message.encode();
  return connection.send(channel, 0, 0, payload.data(), payload.size());
}

class FakeSession final : public rcnet::SessionHandler {
 public:
  FakeSession(std::wstring outputPath, std::string serverName, std::string serviceId)
      : outputPath_(std::move(outputPath)),
        serverName_(std::move(serverName)),
        serviceId_(std::move(serviceId)) {}

  ~FakeSession() override {
    if (output_ != nullptr) std::fclose(output_);
  }

  void onConnected(rcnet::Connection& connection) override {
    nextFormatGeneration_ = 1;
    pendingFormatGeneration_ = 0;
    std::printf("phone connected from %s; waiting for hello\n", connection.peer().c_str());
  }

  void onFrame(rcnet::Connection& connection, const rc::wire::Frame& frame) override {
    if (frame.channel == static_cast<uint8_t>(rc::wire::Channel::Audio)) return;
    if (frame.channel == static_cast<uint8_t>(rc::wire::Channel::Video)) {
      handleVideo(frame);
      return;
    }
    if (frame.channel != static_cast<uint8_t>(rc::wire::Channel::Control) &&
        frame.channel != static_cast<uint8_t>(rc::wire::Channel::Stats)) {
      std::printf("ignoring unknown channel %u\n", frame.channel);
      return;
    }

    rc::control::Message message;
    rc::cbor::Error cborError = rc::cbor::Error::None;
    const rc::control::Error error =
        rc::control::Message::decode(frame.payload, message, cborError);
    if (error != rc::control::Error::None) {
      std::printf("dropped malformed control message: %s (%s)\n",
                  rc::control::errorText(error), rc::cbor::errorText(cborError));
      return;
    }

    std::printf("control: %s (%zu fields)\n", message.type.c_str(), message.fields.size());
    if (message.type == "hello") {
      handleHello(connection, message);
    } else if (message.type == "caps") {
      handleCaps(connection, message);
    } else if (message.type == "orientation") {
      rc::control::Orientation value;
      if (rc::control::parseOrientation(message, value)) {
        std::printf("  orientation %.1f degrees%s\n", value.degrees,
                    value.locked ? " (locked)" : "");
      }
    } else if (message.type == "thermal") {
      rc::control::Thermal value;
      if (rc::control::parseThermal(message, value)) {
        std::printf("  thermal %s\n", value.state.c_str());
      }
    } else if (message.type == "battery") {
      rc::control::Battery value;
      if (rc::control::parseBattery(message, value)) {
        std::printf("  battery %.0f%%%s\n", value.level * 100.0,
                    value.charging ? " charging" : "");
      }
    } else if (message.type == "camera_state") {
      std::string lens;
      double zoom = 0.0;
      message.text("lens", lens);
      message.number("zoom", zoom);
      std::printf("  camera lens=%s zoom=%.2f\n", lens.c_str(), zoom);
    } else if (message.type == "stream_start") {
      std::printf("  stream started; access units -> %ls\n", outputPath_.c_str());
    } else if (message.type == "format_ack") {
      uint64_t generation = 0;
      if (rc::control::parseFormatAck(message, generation) &&
          generation == pendingFormatGeneration_) {
        pendingFormatGeneration_ = 0;
        sendControl(connection, rc::control::requestKeyframe());
        std::printf("  format generation %llu active; requested recovery keyframe\n",
                    static_cast<unsigned long long>(generation));
      } else {
        std::printf("  unexpected format acknowledgement\n");
      }
    } else if (message.type == "format_reject") {
      rc::control::FormatReject rejected;
      if (rc::control::parseFormatReject(message, rejected) &&
          rejected.generation == pendingFormatGeneration_) {
        pendingFormatGeneration_ = 0;
        sendControl(connection, rc::control::requestKeyframe());
        std::printf("  format generation %llu rejected (%s): %s\n",
                    static_cast<unsigned long long>(rejected.generation),
                    rejected.code.c_str(), rejected.message.c_str());
      }
    } else if (message.type == "error") {
      rc::control::DeviceError value;
      rc::control::parseError(message, value);
      std::printf("  phone error %s: %s\n", value.code.c_str(), value.message.c_str());
    }
  }

  void onDisconnected(rcnet::Connection& connection, HRESULT reason) override {
    std::printf("phone %s disconnected: %ls\n", connection.peer().c_str(),
                rcwin::hrMessage(reason).c_str());
    if (output_ != nullptr) std::fflush(output_);
  }

 private:
  void handleHello(rcnet::Connection& connection, const rc::control::Message& message) {
    rc::control::Hello hello;
    if (!rc::control::parseHello(message, hello)) {
      std::printf("  invalid hello (v and device_id are required)\n");
      return;
    }
    std::printf("  %s / %s / %s, id %s, protocol %llu\n", hello.deviceName.c_str(),
                hello.platform.c_str(), hello.model.c_str(), hello.deviceId.c_str(),
                static_cast<unsigned long long>(hello.version));
    if (hello.version > rc::control::kProtocolVersion) {
      std::printf("  protocol is newer than this harness; closing\n");
      connection.close();
      return;
    }

    sendControl(connection,
                rc::control::serverInfo(serverName_, serviceId_, false, true,
                                        {"h264", "hevc"}));
    const rc::control::StreamConfig config = rc::control::conservativeDefault();
    codec_ = rc::annexb::Codec::H264;
    const HRESULT readyHr = sendControl(connection, rc::control::ready(config));
    std::printf("  insecure ready 1280x720 h264 30 fps: %ls\n",
                rcwin::hrMessage(readyHr).c_str());
  }

  void handleCaps(rcnet::Connection& connection, const rc::control::Message& message) {
    rc::control::Caps caps;
    if (!rc::control::parseCaps(message, caps)) {
      std::printf("  invalid caps\n");
      return;
    }
    std::printf("  %zu cameras, %zu codecs\n", caps.cameras.size(), caps.codecs.size());
    for (const rc::control::CameraDescriptor& camera : caps.cameras) {
      std::printf("    %s: %s %s (%zu formats)\n", camera.id.c_str(),
                  camera.position.c_str(), camera.lens.c_str(), camera.formats.size());
    }

    // Exercise every PC->phone control family once. The values are deliberately
    // conservative and reversible; this is a harness, not a UI policy.
    sendControl(connection, rc::control::setPreview(false));
    sendControl(connection, rc::control::setPreview(true));
    if (!caps.cameras.empty()) {
      const rc::control::CameraDescriptor& first = caps.cameras.front();
      sendControl(connection, rc::control::setCamera(
                                  first.lens, first.position.empty()
                                                  ? std::optional<std::string>{}
                                                  : std::optional<std::string>{first.position}));
    }

    rc::control::CameraControls controls;
    controls.zoom = 1.0;
    controls.focusMode = "auto";
    controls.exposureMode = "auto";
    controls.whiteBalanceMode = "auto";
    controls.stabilization = true;
    sendControl(connection, rc::control::setControl(controls));

    const rc::control::StreamConfig config = rc::control::conservativeDefault();
    pendingFormatGeneration_ = nextFormatGeneration_++;
    sendControl(connection, rc::control::setFormat(config, pendingFormatGeneration_));
    rc::control::Stats stats;
    stats.targetBitrate = config.bitrate;
    sendControl(connection, rc::control::stats(stats),
                static_cast<uint8_t>(rc::wire::Channel::Stats));
  }

  void handleVideo(const rc::wire::Frame& frame) {
    if (output_ == nullptr) {
      std::FILE* opened = nullptr;
      if (_wfopen_s(&opened, outputPath_.c_str(), L"wb") != 0 || opened == nullptr) {
        std::fwprintf(stderr, L"could not open %ls for video output\n", outputPath_.c_str());
        return;
      }
      output_ = opened;
    }
    if (!frame.payload.empty()) {
      const size_t written =
          std::fwrite(frame.payload.data(), 1, frame.payload.size(), output_);
      if (written != frame.payload.size()) {
        std::fwprintf(stderr, L"short write to %ls\n", outputPath_.c_str());
      }
    }
    ++videoFrames_;

    const rc::annexb::AccessUnitReport report =
        rc::annexb::inspect(frame.payload.data(), frame.payload.size(), codec_);
    if (!report.isAnnexB) {
      std::printf("video %llu is not Annex-B\n",
                  static_cast<unsigned long long>(videoFrames_));
    } else if (frame.isKeyframe() && !report.decodableFromHere) {
      std::printf("KEYFRAME %llu is missing required parameter sets\n",
                  static_cast<unsigned long long>(videoFrames_));
    } else if (frame.isKeyframe()) {
      std::printf("keyframe %llu: %d NALs, pts %llu us, self-contained\n",
                  static_cast<unsigned long long>(videoFrames_), report.nalCount,
                  static_cast<unsigned long long>(frame.ptsMicros));
    }
  }

  std::wstring outputPath_;
  std::string serverName_;
  std::string serviceId_;
  std::FILE* output_ = nullptr;
  rc::annexb::Codec codec_ = rc::annexb::Codec::H264;
  uint64_t videoFrames_ = 0;
  uint64_t nextFormatGeneration_ = 1;
  uint64_t pendingFormatGeneration_ = 0;
};

}  // namespace

int wmain(int argc, wchar_t** argv) {
  Options options;
  if (!parseOptions(argc, argv, options)) return usage();

  rcwin::logInit(L"fakepc");
  ::SetConsoleCtrlHandler(consoleHandler, TRUE);

  const std::wstring displayName = rcnet::computerDisplayName();
  const std::wstring serviceId = rcnet::machineServiceId();
  FakeSession session(options.outputPath, utf8(displayName), utf8(serviceId));
  rcnet::TcpListener listener;
  const HRESULT listenHr = listener.start(options.port, &session);
  if (FAILED(listenHr)) {
    std::fwprintf(stderr, L"could not listen on port %u: %ls\n", options.port,
                  rcwin::hrMessage(listenHr).c_str());
    return 1;
  }

  rcnet::BonjourService bonjour;
  if (options.advertise) {
    const HRESULT dnsHr = bonjour.start(listener.boundPort(), displayName, serviceId);
    if (FAILED(dnsHr)) {
      std::fwprintf(stderr, L"Bonjour unavailable (%ls); manual connection still works\n",
                    rcwin::hrMessage(dnsHr).c_str());
    }
  }

  std::printf("INSECURE DEVELOPMENT RECEIVER on TCP %u\n", listener.boundPort());
  std::printf("Press Ctrl+C to stop.\n");
  while (!g_stop.load(std::memory_order_acquire)) ::Sleep(100);

  bonjour.stop();
  listener.stop();
  return 0;
}

#endif
