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

  if (width <= 0 || height <= 0 || fps <= 0 ||
      static_cast<unsigned>(width) > rcwin::kRingMaxWidth ||
      static_cast<unsigned>(height) > rcwin::kRingMaxHeight) {
    std::wprintf(L"ERROR: geometry out of range (max %ux%u)\n", rcwin::kRingMaxWidth,
                 rcwin::kRingMaxHeight);
    return 1;
  }

  ::SetConsoleCtrlHandler(consoleHandler, TRUE);

  const rcwin::Nv12Layout layout = rcwin::nv12Layout(width, height);
  std::vector<uint8_t> frame(layout.totalSize);

  rcwin::FrameRing ring;
  std::wprintf(L"rc-fakewriter: %dx%d @ %d fps\n", layout.width, layout.height, fps);
  std::wprintf(L"Waiting for a consumer to open the RemoteCam camera...\n");

  bool announced = false;
  uint64_t frameIndex = 0;
  const DWORD intervalMs = static_cast<DWORD>(1000 / fps);

  while (!g_stop) {
    if (!ring.valid()) {
      // ERROR_FILE_NOT_FOUND here just means no application currently has the camera
      // open. Poll at a human pace: this is a person opening Zoom, not a hot path.
      if (FAILED(ring.open(true))) {
        if (announced) {
          std::wprintf(L"Ring went away (camera closed). Waiting...\n");
          announced = false;
        }
        ::Sleep(250);
        continue;
      }
      std::wprintf(L"Ring opened. Publishing frames.\n");
      announced = true;
    }

    rcwin::renderPattern(frame.data(), layout, frameIndex, rcwin::PatternStyle::Writer);

    rcwin::FrameInfo info;
    info.width = static_cast<uint32_t>(layout.width);
    info.height = static_cast<uint32_t>(layout.height);
    info.stride = static_cast<uint32_t>(layout.stride);
    info.format = rcwin::kFourccNv12;
    info.ptsMicros = ::GetTickCount64() * 1000ull;

    const HRESULT hr =
        ring.writeFrame(frame.data(), static_cast<uint32_t>(layout.totalSize), info);
    if (FAILED(hr)) {
      std::wprintf(L"writeFrame failed: %s\n", rcwin::hrMessage(hr).c_str());
      ring.close();
      announced = false;
      ::Sleep(250);
      continue;
    }

    ++frameIndex;
    if (frameIndex % (static_cast<uint64_t>(fps) * 2) == 0) {
      std::wprintf(L"  published %llu frames\n", static_cast<unsigned long long>(frameIndex));
    }
    if (limit > 0 && static_cast<long long>(frameIndex) >= limit) break;

    ::Sleep(intervalMs);
  }

  std::wprintf(L"Stopped after %llu frames.\n", static_cast<unsigned long long>(frameIndex));
  return 0;
}
