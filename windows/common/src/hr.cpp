#include "rcwin/hr.h"

#include <shlobj.h>
#include <strsafe.h>

#include <cstdarg>
#include <mutex>

namespace rcwin {
namespace {

std::mutex g_mutex;
std::wstring g_tag = L"rc";
HANDLE g_file = INVALID_HANDLE_VALUE;
bool g_fileTried = false;
LONGLONG g_written = 0;

const wchar_t* levelName(LogLevel level) {
  switch (level) {
    case LogLevel::Debug: return L"DBG";
    case LogLevel::Info: return L"INF";
    case LogLevel::Warn: return L"WRN";
    case LogLevel::Error: return L"ERR";
  }
  return L"???";
}

// Basename only. Full paths turn every log line into mostly build-directory noise.
const wchar_t* shortFile(const wchar_t* path) {
  const wchar_t* out = path;
  for (const wchar_t* p = path; *p; ++p) {
    if (*p == L'\\' || *p == L'/') out = p + 1;
  }
  return out;
}

// %ProgramData%\RemoteCam\logs -- deliberately not %LOCALAPPDATA%. The Frame Server
// runs as LOCAL SERVICE, whose profile directory is not somewhere a developer will
// think to look, and is not the same place the user-session tools would write to.
std::wstring logDirectory() {
  PWSTR base = nullptr;
  if (FAILED(::SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &base))) return {};
  std::wstring dir(base);
  ::CoTaskMemFree(base);
  dir += L"\\RemoteCam\\logs";
  return dir;
}

// Cap for a single log generation. rc-vcam.dll is loaded by a service that can stay up
// for weeks, so an append-only file with no ceiling is an unbounded disk consumer on
// the user's system drive -- a slow-motion bug that only shows up long after anyone is
// still looking at this code.
constexpr LONGLONG kMaxLogBytes = 4 * 1024 * 1024;

// One previous generation is kept. Two files bound the disk cost at 8 MB while still
// leaving the run *before* the interesting one available, which is usually where the
// cause of a Session 0 failure actually is.
void rotateIfLarge(const std::wstring& path) {
  WIN32_FILE_ATTRIBUTE_DATA info{};
  if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return;

  LARGE_INTEGER size;
  size.HighPart = static_cast<LONG>(info.nFileSizeHigh);
  size.LowPart = info.nFileSizeLow;
  if (size.QuadPart < kMaxLogBytes) return;

  const std::wstring previous = path + L".1";
  ::DeleteFileW(previous.c_str());
  ::MoveFileW(path.c_str(), previous.c_str());
}

// Opened lazily and at most once. A failure here is silent on purpose: logging must
// never be the reason a media source fails to start.
void ensureFile() {
  if (g_fileTried) return;
  g_fileTried = true;

  const std::wstring dir = logDirectory();
  if (dir.empty()) return;

  std::wstring path = dir + L"\\" + g_tag + L".log";
  rotateIfLarge(path);

  HANDLE h = ::CreateFileW(path.c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  g_file = h;

  // Seeded from what is already on disk rather than zero, so the cap bounds the file
  // itself and not merely this process's contribution to it. Appending 4 MB to a file
  // that already held 3.9 MB would otherwise double the intended ceiling.
  LARGE_INTEGER existing{};
  if (::GetFileSizeEx(h, &existing)) g_written = existing.QuadPart;
}

void writeFile(const wchar_t* text) {
  ensureFile();
  if (g_file == INVALID_HANDLE_VALUE) return;

  // UTF-8 on disk so the log opens cleanly in any editor and in CI output.
  const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (bytes <= 1) return;
  std::string utf8(static_cast<size_t>(bytes - 1), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), bytes - 1, nullptr, nullptr);

  DWORD written = 0;
  ::WriteFile(g_file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);

  // Rotation is also checked here, not only at open. rc-vcam.dll can stay loaded for
  // the whole uptime of the Frame Server, so a check that only runs once per load would
  // never fire on exactly the long-running process that needs it. Closing and clearing
  // the "tried" flag makes the next write reopen through ensureFile, which rotates.
  g_written += written;
  if (g_written >= kMaxLogBytes) {
    ::CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
    g_fileTried = false;
    g_written = 0;
  }
}

}  // namespace

void logInit(const wchar_t* tag) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (tag && *tag) g_tag = tag;
}

void logWrite(LogLevel level, const wchar_t* file, int line, const wchar_t* fmt, ...) {
  wchar_t body[1024];
  va_list args;
  va_start(args, fmt);
  ::StringCchVPrintfW(body, ARRAYSIZE(body), fmt, args);
  va_end(args);

  SYSTEMTIME st{};
  ::GetLocalTime(&st);

  wchar_t line_buf[1400];
  ::StringCchPrintfW(line_buf, ARRAYSIZE(line_buf),
                     L"%02u:%02u:%02u.%03u %s [%s] %u/%u %s:%d  %s\r\n", st.wHour, st.wMinute,
                     st.wSecond, st.wMilliseconds, levelName(level), g_tag.c_str(),
                     ::GetCurrentProcessId(), ::GetCurrentThreadId(), shortFile(file), line,
                     body);

  ::OutputDebugStringW(line_buf);

  std::lock_guard<std::mutex> lock(g_mutex);
  writeFile(line_buf);
}

std::wstring hrMessage(HRESULT hr) {
  wchar_t prefix[32];
  ::StringCchPrintfW(prefix, ARRAYSIZE(prefix), L"0x%08X", static_cast<unsigned>(hr));

  PWSTR text = nullptr;
  const DWORD chars = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<PWSTR>(&text), 0, nullptr);
  if (chars == 0 || text == nullptr) return prefix;

  std::wstring message(text, chars);
  ::LocalFree(text);
  while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' ||
                              message.back() == L' ' || message.back() == L'.')) {
    message.pop_back();
  }
  return std::wstring(prefix) + L" (" + message + L")";
}

std::wstring modulePath(HMODULE module) {
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD len =
        ::GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (len == 0) return {};
    if (len < buffer.size()) {
      buffer.resize(len);
      return buffer;
    }
    // Truncated. GetModuleFileNameW gives no length hint, so grow and retry.
    buffer.resize(buffer.size() * 2);
  }
}

}  // namespace rcwin
