#ifndef RCFAKEPHONE_REPORTER_H
#define RCFAKEPHONE_REPORTER_H

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace rcfakephone {

enum class EventLevel { Info, Pass, Warning, Failure };

struct Event {
  EventLevel level = EventLevel::Info;
  std::string kind;
  std::string detail;
  uint64_t elapsedMillis = 0;
};

struct ReportOptions {
  std::filesystem::path jsonlPath;
  std::filesystem::path junitPath;
  bool quiet = false;
};

class Reporter {
 public:
  explicit Reporter(ReportOptions options = {});
  ~Reporter();

  Reporter(const Reporter&) = delete;
  Reporter& operator=(const Reporter&) = delete;

  void event(EventLevel level, std::string kind, std::string detail);
  void info(std::string kind, std::string detail);
  void pass(std::string kind, std::string detail);
  void warning(std::string kind, std::string detail);
  void failure(std::string kind, std::string detail);

  // Writes the JUnit document exactly once. It is safe to call explicitly and is also
  // called by the destructor for early-return paths.
  void finish(const std::string& suiteName);

  int failures() const { return failures_; }
  int warnings() const { return warnings_; }
  bool healthy() const { return healthy_; }
  const std::string& error() const { return error_; }
  const std::vector<Event>& events() const { return events_; }

 private:
  uint64_t elapsedMillis() const;
  void writeJsonl(const Event& value);
  void writeJunit(const std::string& suiteName);

  ReportOptions options_;
  std::chrono::steady_clock::time_point started_ = std::chrono::steady_clock::now();
  std::ofstream jsonl_;
  std::vector<Event> events_;
  int failures_ = 0;
  int warnings_ = 0;
  bool finished_ = false;
  bool healthy_ = true;
  std::string error_;
  std::string suiteName_ = "rc-fakephone";
};

const char* levelName(EventLevel level);

}  // namespace rcfakephone

#endif  // RCFAKEPHONE_REPORTER_H
