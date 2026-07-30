#include "rcfakephone/reporter.h"

#include <cstdio>
#include <sstream>
#include <utility>

namespace rcfakephone {
namespace {

std::string jsonEscape(const std::string& value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const unsigned char character : value) {
    switch (character) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (character < 0x20) {
          char escaped[7] = {};
          std::snprintf(escaped, sizeof(escaped), "\\u%04x", character);
          out += escaped;
        } else {
          out.push_back(static_cast<char>(character));
        }
    }
  }
  return out;
}

std::string xmlEscape(const std::string& value) {
  std::string out;
  for (const char character : value) {
    switch (character) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out.push_back(character); break;
    }
  }
  return out;
}

const char* consolePrefix(EventLevel level) {
  switch (level) {
    case EventLevel::Info: return "INFO";
    case EventLevel::Pass: return "PASS";
    case EventLevel::Warning: return "WARN";
    case EventLevel::Failure: return "FAIL";
  }
  return "INFO";
}

}  // namespace

const char* levelName(EventLevel level) {
  switch (level) {
    case EventLevel::Info: return "info";
    case EventLevel::Pass: return "pass";
    case EventLevel::Warning: return "warning";
    case EventLevel::Failure: return "failure";
  }
  return "info";
}

Reporter::Reporter(ReportOptions options) : options_(std::move(options)) {
  const auto prepareParent = [&](const std::filesystem::path& path) {
    if (path.empty() || path.parent_path().empty()) return;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error && healthy_) {
      healthy_ = false;
      error_ = "cannot create report directory: " + error.message();
    }
  };
  prepareParent(options_.jsonlPath);
  prepareParent(options_.junitPath);
  if (!options_.jsonlPath.empty()) {
    jsonl_.open(options_.jsonlPath, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!jsonl_.is_open() && healthy_) {
      healthy_ = false;
      error_ = "cannot open NDJSON report";
    }
  }
  if (!options_.junitPath.empty()) {
    std::ofstream probe(options_.junitPath,
                        std::ios::out | std::ios::trunc | std::ios::binary);
    if (!probe.is_open() && healthy_) {
      healthy_ = false;
      error_ = "cannot open JUnit report";
    }
  }
}

Reporter::~Reporter() { finish(suiteName_); }

uint64_t Reporter::elapsedMillis() const {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - started_)
                                   .count());
}

void Reporter::event(EventLevel level, std::string kind, std::string detail) {
  Event value{level, std::move(kind), std::move(detail), elapsedMillis()};
  if (level == EventLevel::Failure) ++failures_;
  if (level == EventLevel::Warning) ++warnings_;
  if (!options_.quiet) {
    std::printf("[%s] %s: %s\n", consolePrefix(level), value.kind.c_str(),
                value.detail.c_str());
  }
  writeJsonl(value);
  events_.push_back(std::move(value));
}

void Reporter::info(std::string kind, std::string detail) {
  event(EventLevel::Info, std::move(kind), std::move(detail));
}

void Reporter::pass(std::string kind, std::string detail) {
  event(EventLevel::Pass, std::move(kind), std::move(detail));
}

void Reporter::warning(std::string kind, std::string detail) {
  event(EventLevel::Warning, std::move(kind), std::move(detail));
}

void Reporter::failure(std::string kind, std::string detail) {
  event(EventLevel::Failure, std::move(kind), std::move(detail));
}

void Reporter::writeJsonl(const Event& value) {
  if (!jsonl_.is_open()) return;
  jsonl_ << "{\"schema\":\"rc-fakephone.event.v1\",\"elapsed_ms\":"
         << value.elapsedMillis << ",\"level\":\"" << levelName(value.level)
         << "\",\"kind\":\"" << jsonEscape(value.kind) << "\",\"detail\":\""
         << jsonEscape(value.detail) << "\"}\n";
  jsonl_.flush();
}

void Reporter::writeJunit(const std::string& suiteName) {
  if (options_.junitPath.empty()) return;
  std::ofstream junit(options_.junitPath,
                      std::ios::out | std::ios::trunc | std::ios::binary);
  if (!junit.is_open()) return;
  junit << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  junit << "<testsuite name=\"" << xmlEscape(suiteName) << "\" tests=\""
        << events_.size() << "\" failures=\"" << failures_ << "\">\n";
  for (const Event& value : events_) {
    junit << "  <testcase classname=\"rc-fakephone\" name=\""
          << xmlEscape(value.kind + " @" + std::to_string(value.elapsedMillis) + "ms")
          << "\">";
    if (value.level == EventLevel::Failure) {
      junit << "<failure message=\"" << xmlEscape(value.detail) << "\"/>";
    } else if (value.level == EventLevel::Warning) {
      junit << "<system-out>WARNING: " << xmlEscape(value.detail) << "</system-out>";
    } else {
      junit << "<system-out>" << xmlEscape(value.detail) << "</system-out>";
    }
    junit << "</testcase>\n";
  }
  junit << "</testsuite>\n";
}

void Reporter::finish(const std::string& suiteName) {
  if (finished_) return;
  finished_ = true;
  suiteName_ = suiteName;
  writeJunit(suiteName);
  if (jsonl_.is_open()) jsonl_.close();
}

}  // namespace rcfakephone
