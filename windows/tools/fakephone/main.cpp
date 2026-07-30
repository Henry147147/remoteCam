// rc-fakephone.exe -- full-fidelity wire-level iPhone simulator for RemoteCam.
//
// This is a development/test target and is never installed. It deliberately requires
// --allow-insecure-session before accepting an unsigned `ready`, mirroring the iOS
// Debug launch guard. Production-lock verifies the shipping endpoint stops at the
// paired=false boundary without weakening it.

#include <windows.h>
#include <windns.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "rc/control.h"
#include "rcfakephone/scenario_engine.h"

namespace {

enum ExitCode {
  kSuccess = 0,
  kAssertionFailure = 1,
  kUsageError = 2,
  kConnectionFailure = 3,
  kSecurityBarrier = 4,
};

struct CliOptions {
  std::string command;
  rcfakephone::Endpoint endpoint;
  std::string suite = "smoke";
  std::string scenario = "smoke";
  std::string profile = "standard";
  uint64_t seed = 0x72656d6f74656361ull;
  uint64_t durationMillis = 2000;
  bool allowInsecureSession = false;
  bool realtime = true;
  std::filesystem::path reportJsonl;
  std::filesystem::path reportJunit;
  std::filesystem::path mediaFile;
  std::filesystem::path scriptFile;
  std::string deviceId;
  rc::control::Codec replayCodec = rc::control::Codec::H264;
};

int usage(FILE* output = stderr, int code = kUsageError) {
  std::fwprintf(
      output,
      L"rc-fakephone - RemoteCam wire-level iPhone simulator\n\n"
      L"Usage:\n"
      L"  rc-fakephone suite --connect HOST[:PORT] --suite smoke|conformance|chaos|soak\n"
      L"  rc-fakephone run --connect HOST[:PORT] --scenario NAME\n"
      L"  rc-fakephone run --connect HOST[:PORT] --script FILE.rcscenario\n"
      L"  rc-fakephone replay --connect HOST[:PORT] --file STREAM.h264\n"
      L"  rc-fakephone discover [--duration SECONDS]\n"
      L"  rc-fakephone shell --connect HOST[:PORT]\n\n"
      L"Options:\n"
      L"  --allow-insecure-session   accept unsigned ready (test endpoints only)\n"
      L"  --seed N                   deterministic PCG32 seed\n"
      L"  --duration SECONDS        scenario duration (fractional accepted)\n"
      L"  --profile standard|constrained\n"
      L"  --device-id 16lowerhex\n"
      L"  --codec h264|hevc          replay elementary-stream codec\n"
      L"  --report-jsonl PATH       NDJSON event stream, schema v1\n"
      L"  --report-junit PATH       JUnit XML report\n"
      L"  --no-realtime             send frames as fast as the socket accepts\n\n"
      L"Scenarios: %hs\n\n"
      L"Exit codes: 0 pass, 1 assertion/protocol failure, 2 usage/configuration,\n"
      L"            3 connection failure, 4 secure pairing barrier\n",
      rcfakephone::ScenarioEngine::scenarioList().c_str());
  return code;
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

bool parseUnsigned(const std::wstring& text, uint64_t& out) {
  if (text.empty() || text[0] == L'-') return false;
  wchar_t* end = nullptr;
  errno = 0;
  const unsigned long long value = std::wcstoull(text.c_str(), &end, 0);
  if (errno != 0 || end == text.c_str() || *end != L'\0') return false;
  out = static_cast<uint64_t>(value);
  return true;
}

bool parseDuration(const std::wstring& text, uint64_t& outMillis) {
  if (text.empty() || text[0] == L'-') return false;
  wchar_t* end = nullptr;
  errno = 0;
  const double seconds = std::wcstod(text.c_str(), &end);
  if (errno != 0 || end == text.c_str() || *end != L'\0' || seconds <= 0.0 ||
      seconds > 86400.0) {
    return false;
  }
  outMillis = static_cast<uint64_t>(seconds * 1000.0);
  return outMillis > 0;
}

bool parsePort(const std::string& text, uint16_t& out) {
  if (text.empty()) return false;
  char* end = nullptr;
  errno = 0;
  const unsigned long value = std::strtoul(text.c_str(), &end, 10);
  if (errno != 0 || end == text.c_str() || *end != '\0' || value == 0 || value > 65535) {
    return false;
  }
  out = static_cast<uint16_t>(value);
  return true;
}

bool parseEndpoint(const std::wstring& value, rcfakephone::Endpoint& out) {
  const std::string text = utf8(value);
  if (text.empty()) return false;
  if (text.front() == '[') {
    const size_t close = text.find(']');
    if (close == std::string::npos || close == 1) return false;
    out.host = text.substr(1, close - 1);
    if (close + 1 == text.size()) return true;
    if (text[close + 1] != ':') return false;
    return parsePort(text.substr(close + 2), out.port);
  }

  const size_t colon = text.rfind(':');
  if (colon != std::string::npos && text.find(':') == colon) {
    if (colon == 0 || !parsePort(text.substr(colon + 1), out.port)) return false;
    out.host = text.substr(0, colon);
  } else {
    // An unbracketed string with more than one colon is an IPv6 address with the
    // default port. Brackets are required when supplying its port.
    out.host = text;
  }
  return !out.host.empty();
}

bool parseOptions(int argc, wchar_t** argv, CliOptions& out) {
  if (argc < 2) return false;
  out.command = utf8(argv[1]);
  for (int index = 2; index < argc; ++index) {
    const std::wstring argument(argv[index]);
    const auto value = [&](std::wstring& target) -> bool {
      if (index + 1 >= argc) return false;
      target = argv[++index];
      return true;
    };
    std::wstring next;
    if (argument == L"--connect" && value(next)) {
      if (!parseEndpoint(next, out.endpoint)) return false;
    } else if (argument == L"--suite" && value(next)) {
      out.suite = utf8(next);
    } else if (argument == L"--scenario" && value(next)) {
      out.scenario = utf8(next);
    } else if (argument == L"--profile" && value(next)) {
      out.profile = utf8(next);
    } else if (argument == L"--seed" && value(next)) {
      if (!parseUnsigned(next, out.seed)) return false;
    } else if (argument == L"--duration" && value(next)) {
      if (!parseDuration(next, out.durationMillis)) return false;
    } else if (argument == L"--device-id" && value(next)) {
      out.deviceId = utf8(next);
    } else if (argument == L"--codec" && value(next)) {
      if (!rc::control::codecFromName(utf8(next), out.replayCodec)) return false;
    } else if (argument == L"--report-jsonl" && value(next)) {
      out.reportJsonl = std::filesystem::path(next);
    } else if (argument == L"--report-junit" && value(next)) {
      out.reportJunit = std::filesystem::path(next);
    } else if (argument == L"--file" && value(next)) {
      out.mediaFile = std::filesystem::path(next);
    } else if (argument == L"--script" && value(next)) {
      out.scriptFile = std::filesystem::path(next);
    } else if (argument == L"--allow-insecure-session") {
      out.allowInsecureSession = true;
    } else if (argument == L"--no-realtime") {
      out.realtime = false;
    } else {
      return false;
    }
  }
  return true;
}

rcfakephone::PhoneProfile selectedProfile(const CliOptions& options) {
  rcfakephone::PhoneProfile profile = options.profile == "constrained"
                                          ? rcfakephone::constrainedProfile()
                                          : rcfakephone::standardProfile();
  if (!options.deviceId.empty()) profile.deviceId = options.deviceId;
  return profile;
}

rcfakephone::ScenarioOptions scenarioOptions(const CliOptions& options,
                                             const std::string& scenario) {
  rcfakephone::ScenarioOptions result;
  result.endpoint = options.endpoint;
  result.scenario = scenario;
  result.seed = options.seed;
  result.durationMillis = options.durationMillis;
  result.profile = selectedProfile(options);
  result.allowInsecureSession = options.allowInsecureSession;
  result.realtime = options.realtime;
  result.replayFile = options.mediaFile;
  result.replayCodec = options.replayCodec;
  return result;
}

int resultExit(const rcfakephone::ScenarioResult& result) {
  if (result.passed) return kSuccess;
  if (result.securityBlocked) return kSecurityBarrier;
  if (result.connectionFailed) return kConnectionFailure;
  return kAssertionFailure;
}

int runScenarios(const CliOptions& options, const std::vector<std::string>& scenarios,
                 const std::string& reportName) {
  rcfakephone::Reporter reporter(
      {{options.reportJsonl}, {options.reportJunit}, false});
  if (!reporter.healthy()) {
    std::fprintf(stderr, "rc-fakephone: %s\n", reporter.error().c_str());
    return kUsageError;
  }
  rcfakephone::ScenarioEngine engine(reporter);
  int exitCode = kSuccess;
  for (const std::string& scenario : scenarios) {
    rcfakephone::ScenarioOptions run = scenarioOptions(options, scenario);
    if (scenario == "soak" && options.durationMillis == 2000) run.durationMillis = 60000;
    const int code = resultExit(engine.run(run));
    if (code > exitCode) exitCode = code;
  }
  reporter.finish(reportName);
  return exitCode;
}

bool readScript(const std::filesystem::path& path, const CliOptions& base,
                std::vector<rcfakephone::ScenarioOptions>& runs, std::string& reason) {
  std::ifstream input(path);
  if (!input.is_open()) {
    reason = "cannot open scenario script";
    return false;
  }
  rcfakephone::ScenarioOptions current = scenarioOptions(base, "smoke");
  std::string line;
  int lineNumber = 0;
  while (std::getline(input, line)) {
    ++lineNumber;
    const size_t comment = line.find('#');
    if (comment != std::string::npos) line.erase(comment);
    std::istringstream words(line);
    std::string command;
    words >> command;
    if (command.empty()) continue;
    if (command == "scenario") {
      words >> current.scenario;
      if (!rcfakephone::ScenarioEngine::knownScenario(current.scenario)) {
        reason = "line " + std::to_string(lineNumber) + ": unknown scenario";
        return false;
      }
    } else if (command == "duration_ms") {
      words >> current.durationMillis;
    } else if (command == "seed") {
      words >> current.seed;
    } else if (command == "profile") {
      std::string profile;
      words >> profile;
      if (profile == "standard") current.profile = rcfakephone::standardProfile();
      else if (profile == "constrained") current.profile = rcfakephone::constrainedProfile();
      else {
        reason = "line " + std::to_string(lineNumber) + ": unknown profile";
        return false;
      }
    } else if (command == "orientation") {
      words >> current.profile.orientation.degrees;
    } else if (command == "thermal") {
      words >> current.profile.thermal.state;
    } else if (command == "run") {
      runs.push_back(current);
    } else {
      reason = "line " + std::to_string(lineNumber) + ": unknown command " + command;
      return false;
    }
    if (words.fail() && command != "run") {
      reason = "line " + std::to_string(lineNumber) + ": missing or invalid value";
      return false;
    }
  }
  if (runs.empty()) runs.push_back(current);
  return true;
}

int runScript(const CliOptions& options) {
  std::vector<rcfakephone::ScenarioOptions> runs;
  std::string reason;
  if (!readScript(options.scriptFile, options, runs, reason)) {
    std::fprintf(stderr, "rc-fakephone: %s\n", reason.c_str());
    return kUsageError;
  }
  rcfakephone::Reporter reporter(
      {{options.reportJsonl}, {options.reportJunit}, false});
  if (!reporter.healthy()) {
    std::fprintf(stderr, "rc-fakephone: %s\n", reporter.error().c_str());
    return kUsageError;
  }
  rcfakephone::ScenarioEngine engine(reporter);
  int exitCode = kSuccess;
  for (const rcfakephone::ScenarioOptions& run : runs) {
    const int code = resultExit(engine.run(run));
    if (code > exitCode) exitCode = code;
  }
  reporter.finish("script");
  return exitCode;
}

struct BrowseContext {
  std::mutex mutex;
  std::set<std::wstring> names;

  static void WINAPI callback(DWORD status, void* rawContext, PDNS_RECORD records) {
    if (status != ERROR_SUCCESS || rawContext == nullptr) return;
    auto* context = static_cast<BrowseContext*>(rawContext);
    std::lock_guard<std::mutex> lock(context->mutex);
    for (PDNS_RECORD record = records; record != nullptr; record = record->pNext) {
      if (record->wType == DNS_TYPE_PTR && record->Data.PTR.pNameHost != nullptr) {
        context->names.insert(record->Data.PTR.pNameHost);
      }
    }
  }
};

int discover(uint64_t durationMillis) {
  BrowseContext context;
  DNS_SERVICE_BROWSE_REQUEST request{};
  DNS_SERVICE_CANCEL cancel{};
  request.Version = DNS_QUERY_REQUEST_VERSION1;
  request.InterfaceIndex = 0;
  request.QueryName = const_cast<PWSTR>(L"_remotecam._tcp.local");
  request.pBrowseCallback = &BrowseContext::callback;
  request.pQueryContext = &context;
  const DNS_STATUS status = ::DnsServiceBrowse(&request, &cancel);
  if (status != DNS_REQUEST_PENDING) {
    std::fprintf(stderr, "Bonjour browse failed: %lu\n", static_cast<unsigned long>(status));
    return kConnectionFailure;
  }
  std::printf("Browsing _remotecam._tcp.local for %.1f seconds...\n",
              static_cast<double>(durationMillis) / 1000.0);
  std::this_thread::sleep_for(std::chrono::milliseconds(durationMillis));
  ::DnsServiceBrowseCancel(&cancel);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  std::lock_guard<std::mutex> lock(context.mutex);
  for (const std::wstring& name : context.names) std::wprintf(L"%s\n", name.c_str());
  if (context.names.empty()) {
    std::printf("No RemoteCam services found. Manual --connect remains available.\n");
  }
  return kSuccess;
}

int shell(CliOptions options) {
  std::printf("rc-fakephone shell. Enter a scenario name, 'list', or 'quit'.\n");
  std::string line;
  while (std::printf("fakephone> "), std::getline(std::cin, line)) {
    if (line == "quit" || line == "exit") return kSuccess;
    if (line == "list") {
      std::printf("%s\n", rcfakephone::ScenarioEngine::scenarioList().c_str());
      continue;
    }
    if (!rcfakephone::ScenarioEngine::knownScenario(line)) {
      std::printf("unknown scenario; enter 'list'\n");
      continue;
    }
    options.scenario = line;
    const int code = runScenarios(options, {line}, "shell-" + line);
    std::printf("exit=%d\n", code);
  }
  return kSuccess;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc == 2 && (std::wstring(argv[1]) == L"--help" ||
                    std::wstring(argv[1]) == L"-h" ||
                    std::wstring(argv[1]) == L"help")) {
    return usage(stdout, kSuccess);
  }
  CliOptions options;
  if (!parseOptions(argc, argv, options)) return usage();
  if (options.profile != "standard" && options.profile != "constrained") return usage();
  if (!options.deviceId.empty() && !rcfakephone::validDeviceId(options.deviceId)) return usage();

  if (options.command == "discover") return discover(options.durationMillis);
  if (options.command == "shell") return shell(options);
  if (options.command == "run") {
    if (!options.scriptFile.empty()) return runScript(options);
    if (!rcfakephone::ScenarioEngine::knownScenario(options.scenario)) return usage();
    return runScenarios(options, {options.scenario}, options.scenario);
  }
  if (options.command == "replay") {
    if (options.mediaFile.empty()) return usage();
    return runScenarios(options, {"smoke"}, "replay");
  }
  if (options.command == "suite") {
    if (options.suite == "smoke") return runScenarios(options, {"smoke"}, "smoke");
    if (options.suite == "conformance") {
      return runScenarios(options,
                          {"smoke", "controls", "adaptive", "media-recovery",
                           "wire-conformance"},
                          "conformance");
    }
    if (options.suite == "chaos") return runScenarios(options, {"chaos"}, "chaos");
    if (options.suite == "soak") return runScenarios(options, {"soak"}, "soak");
    return usage();
  }
  return usage();
}
