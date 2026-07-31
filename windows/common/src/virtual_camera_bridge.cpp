#include "rcwin/virtual_camera_bridge.h"

#include <new>

namespace rcwin {
namespace {

class AppOwnedVirtualCameraBridge final : public IVirtualCameraBridge {
 public:
  explicit AppOwnedVirtualCameraBridge(RingNames names) : names_(names) {}

  VirtualCameraBridgeTopology topology() const override {
    return VirtualCameraBridgeTopology::AppOwned;
  }

  HRESULT open() override { return ring_.open(true, names_); }
  void close() override { ring_.close(); }
  bool connected() const override { return ring_.valid(); }

  HRESULT requestedGeometry(uint32_t& width, uint32_t& height,
                            uint32_t& format) const override {
    return ring_.requestedGeometry(width, height, format);
  }

  HRESULT publish(const uint8_t* bytes, uint32_t size,
                  const FrameInfo& info) override {
    return ring_.writeFrame(bytes, size, info);
  }

 private:
  RingNames names_;
  FrameRing ring_;
};

}  // namespace

HRESULT createVirtualCameraBridge(VirtualCameraBridgeTopology topology,
                                  std::unique_ptr<IVirtualCameraBridge>& out,
                                  RingNames names) {
  out.reset();
  switch (topology) {
    case VirtualCameraBridgeTopology::Brokered:
      // This is an architecture boundary, not a dormant service implementation. The
      // direct Session 0 handoff must fail its physical gate before broker work begins.
      return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    case VirtualCameraBridgeTopology::AppOwned:
      break;
    default:
      return E_INVALIDARG;
  }

  auto* bridge = new (std::nothrow) AppOwnedVirtualCameraBridge(names);
  if (bridge == nullptr) return E_OUTOFMEMORY;
  out.reset(bridge);
  return S_OK;
}

}  // namespace rcwin
