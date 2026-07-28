// HRESULT plumbing and logging.
//
// The logging here is not a nicety. rc-vcam.dll is loaded into svchost.exe in Session
// 0, where there is no console, no message box the user will ever see, and no debugger
// attached by default. Without a log the only symptom of any failure is "the camera is
// black", which is indistinguishable between a registration problem, a media type
// negotiation problem and a shared-memory permission problem.
//
// So every component writes to two sinks: OutputDebugStringW (live, via DebugView) and
// a file under %ProgramData%\RemoteCam\logs. The file is what actually gets used --
// LOCAL SERVICE can write there once rc-vcam-register.exe has created the directory
// with a permissive DACL, and it survives the process exiting.

#ifndef RCWIN_HR_H
#define RCWIN_HR_H

#include <windows.h>

#include <string>

namespace rcwin {

enum class LogLevel { Debug, Info, Warn, Error };

// `tag` names the component ("vcam", "register", "probe") and becomes both the log
// filename and a column in every line, so interleaved logs stay readable.
void logInit(const wchar_t* tag);
void logWrite(LogLevel level, const wchar_t* file, int line, const wchar_t* fmt, ...);

// Human-readable form of an HRESULT, falling back to hex when the system has no
// message for it -- which is common for the MF_E_* range.
std::wstring hrMessage(HRESULT hr);

// Absolute path of the current module (the DLL for rc-vcam, the EXE for the tools).
// The register helper needs this to write an absolute InprocServer32 path: a relative
// one resolves against the Frame Server's working directory, not ours.
std::wstring modulePath(HMODULE module = nullptr);

}  // namespace rcwin

#define RC_LOG(fmt, ...) \
  ::rcwin::logWrite(::rcwin::LogLevel::Info, __FILEW__, __LINE__, fmt, ##__VA_ARGS__)
#define RC_DBG(fmt, ...) \
  ::rcwin::logWrite(::rcwin::LogLevel::Debug, __FILEW__, __LINE__, fmt, ##__VA_ARGS__)
#define RC_WARN(fmt, ...) \
  ::rcwin::logWrite(::rcwin::LogLevel::Warn, __FILEW__, __LINE__, fmt, ##__VA_ARGS__)
#define RC_ERR(fmt, ...) \
  ::rcwin::logWrite(::rcwin::LogLevel::Error, __FILEW__, __LINE__, fmt, ##__VA_ARGS__)

// Evaluate once, log with the originating line, and propagate. The do/while wrapper
// keeps these usable as a single statement inside an unbraced if.
#define RC_RETURN_IF_FAILED(expr)                                                   \
  do {                                                                              \
    const HRESULT rc_hr_ = (expr);                                                  \
    if (FAILED(rc_hr_)) {                                                           \
      ::rcwin::logWrite(::rcwin::LogLevel::Error, __FILEW__, __LINE__, L"%s -> %s", \
                        L## #expr, ::rcwin::hrMessage(rc_hr_).c_str());             \
      return rc_hr_;                                                                \
    }                                                                               \
  } while (0)

#define RC_RETURN_HR_IF(cond, hr)                                                     \
  do {                                                                                \
    if (cond) {                                                                       \
      const HRESULT rc_hr_ = (hr);                                                    \
      ::rcwin::logWrite(::rcwin::LogLevel::Error, __FILEW__, __LINE__, L"%s -> %s",   \
                        L## #cond, ::rcwin::hrMessage(rc_hr_).c_str());               \
      return rc_hr_;                                                                  \
    }                                                                                 \
  } while (0)

#define RC_RETURN_IF_NULL(ptr) RC_RETURN_HR_IF((ptr) == nullptr, E_POINTER)

// Last Win32 error as an HRESULT, normalising the "failed but GetLastError says
// success" case that would otherwise return S_OK from a failure path.
#define RC_HR_FROM_LAST_ERROR() (::rcwin::hrFromLastError())

namespace rcwin {
inline HRESULT hrFromLastError() {
  const DWORD err = ::GetLastError();
  return err == ERROR_SUCCESS ? E_FAIL : HRESULT_FROM_WIN32(err);
}
}  // namespace rcwin

#endif  // RCWIN_HR_H
