// rc-fakewriter.exe -- stand-in for the real capture pipeline.
//
// This tool exists to answer exactly one question, which is the make-or-break unknown
// of the whole project: can an ordinary, non-elevated process in the user's interactive
// session get frames into a media source running as LOCAL SERVICE in Session 0?
//
// It writes a visually distinct pattern into the shared-memory ring. If a camera
// consumer switches from rc-vcam's placeholder to this pattern, the Session 0 handoff
// works and the rest of PLAN.md can be built on top of it. If it does not, that is an
// architecture-level result, not a bug to work around.
//
// Note the direction of the handshake. The ring is created by rc-vcam.dll, not by this
// process, because creating a Global\ section requires SeCreateGlobalPrivilege which an
// interactive user does not hold. So there is nothing to open until some application
// has the camera open -- hence the polling loop below rather than a hard failure.

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "rcwin/hr.h"
#include "rcwin/nv12.h"
#include "rcwin/shm_ring.h"
#include "rcwin/test_pattern.h"

namespace {

volatile LONG g_stop = 0;

BOOL WINAPI consoleHandler(DWORD type) {
  if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
    ::InterlockedExchange(&g_stop, 1);
    return TRUE;
  }
  return FALSE;
}

int usage() {
  std::wprintf(
      L"rc-fakewriter -- publish test frames into the RemoteCam shared-memory ring\n"
      L"\n"
      L"  --width N     frame width  (default 1920)\n"
      L"  --height N    frame height (default 1080)\n"
      L"  --fps N       frames per second (default 30)\n"
      L"  --frames N    stop after N frames (default: run until Ctrl+C)\n"
      L"\n"
      L"The ring is created by rc-vcam.dll inside the Frame Server, so an application\n"
      L"must have the RemoteCam camera open before there is anything to write into.\n"
      L"This tool waits for that rather than failing.\n"
      L"\n");
  return 2;
}

LONGLONG qpcTo100ns(LONGLONG ticks, LONGLONG frequency) {
  const LONGLONG wholeSeconds = ticks / frequency;
  const LONGLONG remainder = ticks % frequency;
  return wholeSeconds * 10000000LL + remainder * 10000000LL / frequency;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  rcwin::logInit(L"rc-fakewriter");

  int width = 1920;
  int height = 1080;
  int fps = 30;
  long long limit = -1;

  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    const bool hasValue = i + 1 < argc;
    if (arg == L"--width" && hasValue) {
      width = _wtoi(argv[++i]);
    } else if (arg == L"--height" && hasValue) {
      height = _wtoi(argv[++i]);
    } else if (arg == L"--fps" && hasValue) {
      fps = _wtoi(argv[++i]);
    } else if (arg == L"--frames" && hasValue) {
      limit = _wtoll(argv[++i]);
    } else {
      return usage();
    }
  }

  if (width <= 0 || height <= 0 || (width & 1) != 0 || (height & 1) != 0 ||
      fps <= 0 || fps > 240 || limit < -1 ||
      static_cast<unsigned>(width) > rcwin::kRingMaxWidth ||
      static_cast<unsigned>(height) > rcwin::kRingMaxHeight) {
    std::wprintf(L"ERROR: dimensions must be positive and even (max %ux%u), fps must "
                 L"be 1..240, and --frames must be non-negative.\n",
                 rcwin::kRingMaxWidth, rcwin::kRingMaxHeight);
    return 1;
  }

  ::SetConsoleCtrlHandler(consoleHandler, TRUE);

  // The ring supports exactly one producer: writeFrame advances the write sequence with
  // a plain read-modify-write, so two publishers interleave into the same slot and
  // interleave unrelated streams even though FrameRing's write guard now prevents
  // memory corruption. The one way anybody is actually going to hit it is by running
  // this tool twice, so guard it here where it costs nothing.
  //
  // Local\ rather than Global\: this is a per-session guard between instances of a
  // user-mode tool, and Global\ would need a privilege the tool does not have.
  HANDLE instanceGuard = ::CreateMutexW(nullptr, TRUE, L"Local\\RemoteCam.FakeWriter.Single");
  if (!instanceGuard || ::GetLastError() == ERROR_ALREADY_EXISTS) {
    std::wprintf(L"ERROR: another rc-fakewriter is already running.\n"
                 L"The frame ring supports one producer; a second would corrupt it.\n");
    if (instanceGuard) ::CloseHandle(instanceGuard);
    return 1;
  }

  const rcwin::Nv12Layout layout = rcwin::nv12Layout(width, height);
  std::vector<uint8_t> frame(layout.totalSize);

  rcwin::FrameRing ring;
  std::wprintf(L"rc-fakewriter: %dx%d @ %d fps\n", layout.width, layout.height, fps);
  std::wprintf(L"Waiting for a consumer to open the RemoteCam camera...\n");

  // Paced with a high-resolution waitable timer against an absolute origin, the same
  // way rc-vcam.dll paces its own output.
  //
  // Sleep(1000/fps) would have been shorter, and wrong twice over: the default timer
  // granularity is ~15.6 ms, so a requested 33 ms sleep overshoots, and the error
  // accumulates because each sleep is relative to whenever the last one happened to
  // wake. Measured on the dev box over 90 frames: Sleep(33) delivers 24.1 fps against a
  // 30 fps target, while the loop below delivers 30.0. A tool whose entire job is to
  // demonstrate that frames flow across the session boundary must not itself be the
  // reason they look late.
  HANDLE timer = ::CreateWaitableTimerExW(nullptr, nullptr,
                                          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
  if (!timer) timer = ::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  if (!timer) {
    std::wprintf(L"CreateWaitableTimerEx failed: %s\n",
                 rcwin::hrMessage(rcwin::hrFromLastError()).c_str());
    ::ReleaseMutex(instanceGuard);
    ::CloseHandle(instanceGuard);
    return 1;
  }

  const LONGLONG intervalTicks = 10000000LL / fps;  // 100 ns units
  LARGE_INTEGER originQpc{};
  LARGE_INTEGER qpcFreq{};
  if (!::QueryPerformanceFrequency(&qpcFreq) || qpcFreq.QuadPart <= 0 ||
      !::QueryPerformanceCounter(&originQpc)) {
    std::wprintf(L"High-resolution performance counter is unavailable.\n");
    ::CloseHandle(timer);
    ::ReleaseMutex(instanceGuard);
    ::CloseHandle(instanceGuard);
    return 1;
  }

  bool announced = false;
  uint64_t frameIndex = 0;
  uint64_t tick = 0;

  int exitCode = 0;
  while (!g_stop && (limit < 0 || static_cast<long long>(frameIndex) < limit)) {
    if (!ring.valid()) {
      // ERROR_FILE_NOT_FOUND here just means no application currently has the camera
      // open. Poll at a human pace: this is a person opening Zoom, not a hot path.
      const HRESULT openHr = ring.open(true);
      if (FAILED(openHr)) {
        if (openHr != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
          std::wprintf(L"FrameRing::open failed: %s\n",
                       rcwin::hrMessage(openHr).c_str());
          exitCode = 1;
          break;
        }
        if (announced) {
          std::wprintf(L"Ring went away (camera closed). Waiting...\n");
          announced = false;
        }
        ::Sleep(250);
        continue;
      }
      std::wprintf(L"Ring opened. Publishing frames.\n");
      announced = true;
      // Restart the schedule from here. The ring can be closed for minutes while nobody
      // has the camera open; carrying the old origin across that gap would leave the
      // timer owing hundreds of frames and publish them back-to-back on reconnect.
      if (!::QueryPerformanceCounter(&originQpc)) {
        std::wprintf(L"QueryPerformanceCounter failed.\n");
        exitCode = 1;
        break;
      }
      tick = 0;
    }

    rcwin::renderPattern(frame.data(), layout, frameIndex, rcwin::PatternStyle::Writer);

    rcwin::FrameInfo info;
    info.width = static_cast<uint32_t>(layout.width);
    info.height = static_cast<uint32_t>(layout.height);
    info.stride = static_cast<uint32_t>(layout.stride);
    info.format = rcwin::kFourccNv12;
    info.ptsMicros = ::GetTickCount64() * 1000ull;
    info.bytesUsed = static_cast<uint32_t>(layout.totalSize);

    const HRESULT hr =
        ring.writeFrame(frame.data(), static_cast<uint32_t>(layout.totalSize), info);
    if (FAILED(hr)) {
      if (hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT)) {
        // The frame path is deliberately non-blocking; pace this dropped frame below.
      } else if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) {
        if (announced) std::wprintf(L"Ring went away (camera closed). Waiting...\n");
        ring.close();
        announced = false;
        ::Sleep(250);
        continue;
      } else {
        std::wprintf(L"writeFrame failed: %s\n", rcwin::hrMessage(hr).c_str());
        exitCode = 1;
        break;
      }
    }

    if (SUCCEEDED(hr)) {
      ++frameIndex;
      if (frameIndex % (static_cast<uint64_t>(fps) * 2) == 0) {
        std::wprintf(L"  published %llu frames\n",
                     static_cast<unsigned long long>(frameIndex));
      }
    }
    // Scheduled from a fixed origin rather than "now + interval", so a late wake-up
    // does not push every subsequent frame later.
    ++tick;
    LARGE_INTEGER now{};
    if (!::QueryPerformanceCounter(&now)) {
      std::wprintf(L"QueryPerformanceCounter failed.\n");
      exitCode = 1;
      break;
    }
    const LONGLONG elapsed100ns =
        qpcTo100ns(now.QuadPart - originQpc.QuadPart, qpcFreq.QuadPart);
    LONGLONG delta = static_cast<LONGLONG>(tick) * intervalTicks - elapsed100ns;
    if (delta < 0) delta = 0;

    LARGE_INTEGER due;
    due.QuadPart = -delta;  // negative == relative
    if (!::SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      std::wprintf(L"SetWaitableTimer failed: %s\n",
                   rcwin::hrMessage(rcwin::hrFromLastError()).c_str());
      exitCode = 1;
      break;
    }
    if (::WaitForSingleObject(timer, INFINITE) != WAIT_OBJECT_0) {
      std::wprintf(L"WaitForSingleObject failed: %s\n",
                   rcwin::hrMessage(rcwin::hrFromLastError()).c_str());
      exitCode = 1;
      break;
    }
  }

  ::CloseHandle(timer);
  ::ReleaseMutex(instanceGuard);
  ::CloseHandle(instanceGuard);
  std::wprintf(L"Stopped after %llu frames.\n", static_cast<unsigned long long>(frameIndex));
  return exitCode;
}
