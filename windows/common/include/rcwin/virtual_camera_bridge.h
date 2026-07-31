// Producer-side boundary for publishing frames to the Windows virtual camera.
//
// The shipping topology is app-owned: rc-vcam.dll creates the Global\ objects from
// Session 0 and the unelevated desktop app opens them. If physical Session 0 testing
// proves that topology cannot work on supported Windows builds, a broker can implement
// this interface without changing the decoder/pipeline producer. Brokered mode is a
// deliberate, currently unsupported boundary -- it must not become an installed
// service merely because the direct path has not yet been tested on real hardware.

#ifndef RCWIN_VIRTUAL_CAMERA_BRIDGE_H
#define RCWIN_VIRTUAL_CAMERA_BRIDGE_H

#include <windows.h>

#include <cstdint>
#include <memory>

#include "rcwin/shm_ring.h"

namespace rcwin {

enum class VirtualCameraBridgeTopology {
  AppOwned,
  Brokered,
};

class IVirtualCameraBridge {
 public:
  virtual ~IVirtualCameraBridge() = default;

  virtual VirtualCameraBridgeTopology topology() const = 0;

  // Non-blocking attach to the bridge. ERROR_FILE_NOT_FOUND is the ordinary state
  // while no camera consumer has caused the Frame Server to create the Global\ ring.
  virtual HRESULT open() = 0;
  virtual void close() = 0;
  virtual bool connected() const = 0;

  virtual HRESULT requestedGeometry(uint32_t& width, uint32_t& height,
                                    uint32_t& format) const = 0;
  virtual HRESULT publish(const uint8_t* bytes, uint32_t size,
                          const FrameInfo& info) = 0;
};

// Creates the currently supported app-owned bridge. `names` is injectable so tests
// can use Local\ objects without SeCreateGlobalPrivilege. Brokered mode returns
// ERROR_NOT_SUPPORTED until the physical Session 0 gate documented in API-NOTES.md
// proves that a service is actually necessary.
HRESULT createVirtualCameraBridge(
    VirtualCameraBridgeTopology topology,
    std::unique_ptr<IVirtualCameraBridge>& out,
    RingNames names = globalRingNames());

}  // namespace rcwin

#endif  // RCWIN_VIRTUAL_CAMERA_BRIDGE_H
