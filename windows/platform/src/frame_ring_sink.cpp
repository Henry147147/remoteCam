#include "rcplatform/frame_ring_sink.h"

#include <d3d10.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "rcplatform/pixel_convert.h"
#include "rcwin/hr.h"
#include "rcwin/nv12.h"
#include "rcwin/virtual_camera_bridge.h"

namespace rcplatform {
namespace {

using Microsoft::WRL::ComPtr;

constexpr size_t kStagingSlots = 3;
constexpr HRESULT kRingNotFound = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
constexpr HRESULT kProducerExists = HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);

bool supportedFrame(const TextureFrame& frame, D3D11_TEXTURE2D_DESC& description) {
  if (!frame.texture || frame.width == 0 || frame.height == 0 ||
      (frame.width & 1u) != 0 || (frame.height & 1u) != 0 ||
      frame.width > rcwin::kRingMaxWidth || frame.height > rcwin::kRingMaxHeight) {
    return false;
  }
  frame.texture->GetDesc(&description);
  return description.Format == DXGI_FORMAT_B8G8R8A8_UNORM &&
         frame.arraySlice < description.ArraySize && description.Width == frame.width &&
         description.Height == frame.height;
}

}  // namespace

struct FrameRingSink::Impl {
  struct Slot {
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11Query> completion;
    uint64_t ptsMicros = 0;
    bool pending = false;
  };

  HRESULT configure(const TextureFrame& frame, const D3D11_TEXTURE2D_DESC& sourceDesc) {
    ComPtr<ID3D11Device> sourceDevice;
    frame.texture->GetDevice(&sourceDevice);
    if (!sourceDevice) return E_FAIL;
    if (device.Get() == sourceDevice.Get() && width == frame.width && height == frame.height) {
      return S_OK;
    }

    pending.clear();
    for (Slot& slot : slots) {
      slot = Slot{};
    }
    context.Reset();
    device = sourceDevice;
    device->GetImmediateContext(&context);
    if (!context) return E_FAIL;

    ComPtr<ID3D10Multithread> multithread;
    if (SUCCEEDED(context.As(&multithread))) multithread->SetMultithreadProtected(TRUE);

    D3D11_TEXTURE2D_DESC stagingDesc = sourceDesc;
    stagingDesc.ArraySize = 1;
    stagingDesc.MipLevels = 1;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;
    stagingDesc.SampleDesc.Count = 1;
    stagingDesc.SampleDesc.Quality = 0;

    D3D11_QUERY_DESC queryDesc{};
    queryDesc.Query = D3D11_QUERY_EVENT;
    for (Slot& slot : slots) {
      HRESULT hr = device->CreateTexture2D(&stagingDesc, nullptr, &slot.staging);
      if (FAILED(hr)) return hr;
      hr = device->CreateQuery(&queryDesc, &slot.completion);
      if (FAILED(hr)) return hr;
    }
    width = frame.width;
    height = frame.height;
    return S_OK;
  }

  void transition(FrameRingSinkState state, HRESULT error = S_OK) {
    Observer callback;
    FrameRingSinkSnapshot current;
    {
      std::lock_guard<std::mutex> lock(statusMutex);
      const bool changed = status.state != state || status.error != error ||
                           status.width != desiredWidth.load(std::memory_order_relaxed) ||
                           status.height != desiredHeight.load(std::memory_order_relaxed);
      status.state = state;
      status.error = error;
      status.width = desiredWidth.load(std::memory_order_relaxed);
      status.height = desiredHeight.load(std::memory_order_relaxed);
      if (!changed) return;
      current = status;
      callback = observer;
    }
    if (callback) callback(current);
  }

  void countDrop() {
    std::lock_guard<std::mutex> lock(statusMutex);
    ++status.droppedFrames;
  }

  void countPublished() {
    std::lock_guard<std::mutex> lock(statusMutex);
    ++status.publishedFrames;
  }

  void run(std::stop_token stopToken) {
    std::unique_ptr<rcwin::IVirtualCameraBridge> bridge;
    const HRESULT bridgeHr = rcwin::createVirtualCameraBridge(
        rcwin::VirtualCameraBridgeTopology::AppOwned, bridge);
    if (FAILED(bridgeHr)) {
      transition(FrameRingSinkState::Failed, bridgeHr);
      return;
    }
    std::vector<uint8_t> bgra;
    std::vector<uint8_t> nv12;

    while (!stopToken.stop_requested()) {
      size_t index = 0;
      uint32_t frameWidth = 0;
      uint32_t frameHeight = 0;
      uint64_t ptsMicros = 0;
      {
        std::unique_lock<std::mutex> lock(slotsMutex);
        wake.wait(lock, stopToken, [this] { return !pending.empty(); });
        if (stopToken.stop_requested()) break;
        index = pending.front();

        HRESULT ready = S_FALSE;
        {
          std::lock_guard<std::mutex> contextLock(contextMutex);
          ready = context->GetData(slots[index].completion.Get(), nullptr, 0,
                                   D3D11_ASYNC_GETDATA_DONOTFLUSH);
        }
        if (ready == S_FALSE) {
          lock.unlock();
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
        if (FAILED(ready)) {
          slots[index].pending = false;
          pending.pop_front();
          countDrop();
          transition(FrameRingSinkState::Failed, ready);
          continue;
        }

        D3D11_MAPPED_SUBRESOURCE mapped{};
        {
          std::lock_guard<std::mutex> contextLock(contextMutex);
          const HRESULT mapHr = context->Map(slots[index].staging.Get(), 0,
                                             D3D11_MAP_READ, 0, &mapped);
          if (FAILED(mapHr)) {
            slots[index].pending = false;
            pending.pop_front();
            countDrop();
            transition(FrameRingSinkState::Failed, mapHr);
            continue;
          }
          frameWidth = width;
          frameHeight = height;
          const size_t packedStride = static_cast<size_t>(frameWidth) * 4u;
          bgra.resize(packedStride * frameHeight);
          const auto* source = static_cast<const uint8_t*>(mapped.pData);
          for (uint32_t row = 0; row < frameHeight; ++row) {
            std::memcpy(bgra.data() + static_cast<size_t>(row) * packedStride,
                        source + static_cast<size_t>(row) * mapped.RowPitch, packedStride);
          }
          context->Unmap(slots[index].staging.Get(), 0);
        }
        ptsMicros = slots[index].ptsMicros;
        slots[index].pending = false;
        pending.pop_front();
      }

      const rcwin::Nv12Layout layout =
          rcwin::nv12Layout(static_cast<int>(frameWidth), static_cast<int>(frameHeight));
      auto preview = std::make_shared<BgraPreviewFrame>();
      preview->pixels.swap(bgra);
      preview->width = frameWidth;
      preview->height = frameHeight;
      preview->stride = frameWidth * 4u;
      preview->ptsMicros = ptsMicros;
      PreviewObserver previewCallback;
      {
        std::lock_guard<std::mutex> lock(statusMutex);
        previewCallback = previewObserver;
      }
      if (previewCallback) previewCallback(preview);

      nv12.resize(layout.totalSize);
      const HRESULT convertHr =
          bgraToNv12(preview->pixels.data(), preview->pixels.size(), preview->stride,
                     frameWidth, frameHeight,
                     nv12.data(), nv12.size(), static_cast<uint32_t>(layout.stride));
      if (FAILED(convertHr)) {
        countDrop();
        transition(FrameRingSinkState::Failed, convertHr);
        continue;
      }

      if (!bridge->connected()) {
        const HRESULT openHr = bridge->open();
        if (FAILED(openHr)) {
          countDrop();
          transition(openHr == kProducerExists ? FrameRingSinkState::ProducerConflict
                                                : FrameRingSinkState::WaitingForConsumer,
                     openHr == kRingNotFound ? S_OK : openHr);
          continue;
        }
      }

      uint32_t requestedWidth = frameWidth;
      uint32_t requestedHeight = frameHeight;
      uint32_t requestedFormat = rcwin::kFourccNv12;
      if (bridge->requestedGeometry(requestedWidth, requestedHeight, requestedFormat) == S_OK &&
          requestedFormat == rcwin::kFourccNv12) {
        desiredWidth.store(requestedWidth, std::memory_order_relaxed);
        desiredHeight.store(requestedHeight, std::memory_order_relaxed);
      }
      if (requestedFormat != rcwin::kFourccNv12 || requestedWidth != frameWidth ||
          requestedHeight != frameHeight) {
        countDrop();
        transition(FrameRingSinkState::AdaptingGeometry);
        continue;
      }

      rcwin::FrameInfo info;
      info.width = frameWidth;
      info.height = frameHeight;
      info.stride = static_cast<uint32_t>(layout.stride);
      info.format = rcwin::kFourccNv12;
      info.ptsMicros = ptsMicros;
      info.bytesUsed = static_cast<uint32_t>(nv12.size());
      const HRESULT writeHr =
          bridge->publish(nv12.data(), static_cast<uint32_t>(nv12.size()), info);
      if (writeHr == kRingNotFound) {
        bridge->close();
        countDrop();
        transition(FrameRingSinkState::WaitingForConsumer);
      } else if (FAILED(writeHr)) {
        countDrop();
        transition(FrameRingSinkState::Failed, writeHr);
      } else {
        countPublished();
        transition(FrameRingSinkState::Publishing);
      }
    }
    bridge->close();
  }

  mutable std::mutex statusMutex;
  FrameRingSinkSnapshot status;
  Observer observer;
  PreviewObserver previewObserver;
  std::atomic<uint32_t> desiredWidth{1920};
  std::atomic<uint32_t> desiredHeight{1080};

  std::mutex slotsMutex;
  std::mutex contextMutex;
  std::condition_variable_any wake;
  std::array<Slot, kStagingSlots> slots;
  std::deque<size_t> pending;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  uint32_t width = 0;
  uint32_t height = 0;
  std::jthread worker;
};

FrameRingSink::FrameRingSink() : impl_(std::make_unique<Impl>()) {}
FrameRingSink::~FrameRingSink() { stop(); }

void FrameRingSink::start() {
  if (impl_->worker.joinable()) return;
  impl_->transition(FrameRingSinkState::WaitingForConsumer);
  impl_->worker = std::jthread([this](std::stop_token token) { impl_->run(token); });
}

void FrameRingSink::stop() {
  if (!impl_->worker.joinable()) return;
  impl_->worker.request_stop();
  impl_->wake.notify_all();
  impl_->worker.join();
  {
    std::lock_guard<std::mutex> lock(impl_->slotsMutex);
    impl_->pending.clear();
    for (Impl::Slot& slot : impl_->slots) slot = Impl::Slot{};
    impl_->context.Reset();
    impl_->device.Reset();
    impl_->width = 0;
    impl_->height = 0;
  }
  impl_->transition(FrameRingSinkState::Stopped);
}

void FrameRingSink::setObserver(Observer observer) {
  FrameRingSinkSnapshot current;
  Observer callback;
  {
    std::lock_guard<std::mutex> lock(impl_->statusMutex);
    impl_->observer = std::move(observer);
    current = impl_->status;
    callback = impl_->observer;
  }
  if (callback) callback(current);
}

void FrameRingSink::setPreviewObserver(PreviewObserver observer) {
  std::lock_guard<std::mutex> lock(impl_->statusMutex);
  impl_->previewObserver = std::move(observer);
}

FrameRingSinkSnapshot FrameRingSink::snapshot() const {
  std::lock_guard<std::mutex> lock(impl_->statusMutex);
  return impl_->status;
}

std::pair<uint32_t, uint32_t> FrameRingSink::desiredGeometry() const {
  return {impl_->desiredWidth.load(std::memory_order_relaxed),
          impl_->desiredHeight.load(std::memory_order_relaxed)};
}

HRESULT FrameRingSink::publish(const TextureFrame& frame) {
  D3D11_TEXTURE2D_DESC sourceDesc{};
  if (!supportedFrame(frame, sourceDesc)) return E_INVALIDARG;
  if (!impl_->worker.joinable()) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);

  std::lock_guard<std::mutex> lock(impl_->slotsMutex);
  HRESULT hr = impl_->configure(frame, sourceDesc);
  if (FAILED(hr)) {
    impl_->transition(FrameRingSinkState::Failed, hr);
    return hr;
  }

  size_t available = kStagingSlots;
  for (size_t index = 0; index < impl_->slots.size(); ++index) {
    if (!impl_->slots[index].pending) {
      available = index;
      break;
    }
  }
  if (available == kStagingSlots) {
    impl_->countDrop();
    return S_FALSE;
  }

  Impl::Slot& slot = impl_->slots[available];
  {
    std::lock_guard<std::mutex> contextLock(impl_->contextMutex);
    const UINT sourceSubresource =
        D3D11CalcSubresource(0, frame.arraySlice, sourceDesc.MipLevels);
    impl_->context->CopySubresourceRegion(slot.staging.Get(), 0, 0, 0, 0,
                                          frame.texture.Get(), sourceSubresource, nullptr);
    impl_->context->End(slot.completion.Get());
  }
  slot.ptsMicros = frame.ptsMicros;
  slot.pending = true;
  impl_->pending.push_back(available);
  impl_->wake.notify_one();
  return S_OK;
}

}  // namespace rcplatform
