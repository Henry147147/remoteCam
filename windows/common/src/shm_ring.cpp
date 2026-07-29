#include "rcwin/shm_ring.h"

#include <sddl.h>

#include <atomic>
#include <cstring>
#include <new>

#include "rcwin/guids.h"
#include "rcwin/hr.h"

namespace rcwin {
namespace {

// Header lives in the first page; slots start on a page boundary so a slot memcpy is
// never split across the header's cache lines.
constexpr uint32_t kHeaderBytes = 4096u;
constexpr uint64_t kSectionBytes =
    static_cast<uint64_t>(kHeaderBytes) + static_cast<uint64_t>(kRingSlotBytes) * kRingSlots;

// SY  LOCAL SYSTEM              full
// BA  Administrators            full
// LS  LOCAL SERVICE             full -- the Frame Server identity, which creates this
// IU  INTERACTIVE               read + write, so an unelevated producer can publish
// AC  ALL APPLICATION PACKAGES  read, so packaged consumers (Windows Camera) can read
//
// "P" makes the DACL protected: no inherited ACEs, so nothing in the object namespace
// can widen this after the fact.
constexpr wchar_t kSddl[] =
    L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;GRGWGX;;;IU)(A;;GRGX;;;AC)";

struct RingHeader {
  std::atomic<uint32_t> magic;
  uint32_t version;
  uint32_t slotCount;
  uint32_t slotBytes;

  std::atomic<uint32_t> formatGeneration;
  std::atomic<uint32_t> width;
  std::atomic<uint32_t> height;
  std::atomic<uint32_t> stride;
  std::atomic<uint32_t> format;
  // The named mapping outlives its creator while a producer still has it open. These
  // fields make consumer lifetime explicit instead of mistaking that stale kernel
  // object for an open camera. A writer binds to one generation.
  std::atomic<uint32_t> consumerCount;
  std::atomic<uint32_t> consumerGeneration;
  uint32_t reserved0;

  std::atomic<uint64_t> writeSeq;      // frames ever published; 0 means "none yet"
  std::atomic<uint64_t> lastWriteQpc;

  std::atomic<uint64_t> slotSeq[kRingSlots];   // even = stable, odd = being written
  std::atomic<uint64_t> slotPts[kRingSlots];
  std::atomic<uint32_t> slotBytesUsed[kRingSlots];
  std::atomic<uint32_t> slotWidth[kRingSlots];
  std::atomic<uint32_t> slotHeight[kRingSlots];
  std::atomic<uint32_t> slotStride[kRingSlots];
};

static_assert(sizeof(RingHeader) <= kHeaderBytes, "ring header outgrew its page");
static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "the seqlock requires lock-free 64-bit atomics; a lock-based atomic would "
              "embed a mutex in shared memory, which is not valid across processes");

// Is this frame's geometry self-consistent and within the ring's limits?
//
// The header and slot metadata live in memory another process writes, and the DACL
// deliberately grants write access to any interactive user. So these fields are input,
// not state -- a producer that reports stride = 0x40000000 would otherwise walk every
// consumer of this ring off the end of its own buffer. The first such consumer is
// rc-vcam.dll running inside svchost.exe, where an access violation takes down the
// Frame Server for every camera application on the machine.
//
// Validated here rather than in each consumer because this is the chokepoint: the OBS
// plugin and the Qt preview will read the same ring later, and a check they each have
// to remember to repeat is a check that will eventually be forgotten.
bool geometryIsSane(const FrameInfo& info) {
  if (info.width == 0 || info.height == 0) return false;
  if (info.width > kRingMaxWidth || info.height > kRingMaxHeight) return false;
  // NV12 is 4:2:0; odd dimensions have no valid chroma plane.
  if (((info.width | info.height) & 1u) != 0) return false;
  if (info.stride < info.width) return false;
  if (info.format != kFourccNv12) return false;

  // 64-bit throughout: stride and height are both attacker-controlled uint32, and the
  // product overflows 32 bits long before it stops being plausible.
  const uint64_t plane = static_cast<uint64_t>(info.stride) * info.height;
  const uint64_t total = plane + plane / 2;
  return total <= info.bytesUsed && info.bytesUsed <= kRingSlotBytes;
}

RingHeader* headerOf(void* view) { return static_cast<RingHeader*>(view); }

uint8_t* slotOf(void* view, uint32_t index) {
  return static_cast<uint8_t*>(view) + kHeaderBytes +
         static_cast<size_t>(index) * kRingSlotBytes;
}

uint64_t qpcNow() {
  LARGE_INTEGER t{};
  ::QueryPerformanceCounter(&t);
  return static_cast<uint64_t>(t.QuadPart);
}

uint64_t qpcFrequency() {
  static const uint64_t freq = [] {
    LARGE_INTEGER f{};
    ::QueryPerformanceFrequency(&f);
    return f.QuadPart > 0 ? static_cast<uint64_t>(f.QuadPart) : 1ull;
  }();
  return freq;
}

bool hasActiveConsumer(const RingHeader* h) {
  const uint32_t count = h->consumerCount.load(std::memory_order_acquire);
  return count != 0;
}

void resetPublishedFrames(RingHeader* h) {
  h->formatGeneration.store(0, std::memory_order_relaxed);
  h->width.store(0, std::memory_order_relaxed);
  h->height.store(0, std::memory_order_relaxed);
  h->stride.store(0, std::memory_order_relaxed);
  h->format.store(0, std::memory_order_relaxed);
  h->writeSeq.store(0, std::memory_order_relaxed);
  h->lastWriteQpc.store(0, std::memory_order_relaxed);
  for (uint32_t slot = 0; slot < kRingSlots; ++slot) {
    h->slotSeq[slot].store(0, std::memory_order_relaxed);
    h->slotPts[slot].store(0, std::memory_order_relaxed);
    h->slotBytesUsed[slot].store(0, std::memory_order_relaxed);
    h->slotWidth[slot].store(0, std::memory_order_relaxed);
    h->slotHeight[slot].store(0, std::memory_order_relaxed);
    h->slotStride[slot].store(0, std::memory_order_relaxed);
  }
}

HRESULT lockWriteGuard(HANDLE guard, DWORD timeoutMillis = 5000) {
  const DWORD wait = ::WaitForSingleObject(guard, timeoutMillis);
  if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) return S_OK;
  if (wait == WAIT_TIMEOUT) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
  return RC_HR_FROM_LAST_ERROR();
}

// Builds SECURITY_ATTRIBUTES from kSddl. The descriptor must outlive the create call,
// so the caller owns it and frees it with LocalFree.
HRESULT makeSecurityAttributes(SECURITY_ATTRIBUTES& sa, PSECURITY_DESCRIPTOR& sd) {
  sd = nullptr;
  if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(kSddl, SDDL_REVISION_1, &sd,
                                                              nullptr)) {
    return RC_HR_FROM_LAST_ERROR();
  }
  sa.nLength = sizeof(sa);
  sa.lpSecurityDescriptor = sd;
  sa.bInheritHandle = FALSE;
  return S_OK;
}

}  // namespace

RingNames globalRingNames() {
  return RingNames{kFrameSectionName, kFrameEventName, kFrameWriteGuardName};
}

RingNames testRingNames() {
  return RingNames{L"Local\\RemoteCam.Test.Frames", L"Local\\RemoteCam.Test.Frame",
                   L"Local\\RemoteCam.Test.FrameWriteGuard"};
}

FrameRing::~FrameRing() { close(); }

void FrameRing::close() {
  if (owner_ && view_) {
    const HRESULT guardHr = lockWriteGuard(writeGuard_);
    RingHeader* h = headerOf(view_);
    const uint32_t previous =
        h->consumerCount.fetch_sub(1, std::memory_order_acq_rel);
    if (previous == 0) {
      // Defensive repair for an unbalanced close; never wrap the shared count.
      h->consumerCount.store(0, std::memory_order_release);
    } else if (previous == 1) {
      RC_LOG(L"last camera consumer closed ring");
    }
    if (SUCCEEDED(guardHr)) {
      ::ReleaseMutex(writeGuard_);
    } else {
      RC_ERR(L"could not lock ring while closing a consumer: %s",
             hrMessage(guardHr).c_str());
    }
  }
  if (view_) {
    ::UnmapViewOfFile(view_);
    view_ = nullptr;
  }
  if (section_) {
    ::CloseHandle(section_);
    section_ = nullptr;
  }
  if (event_) {
    ::CloseHandle(event_);
    event_ = nullptr;
  }
  if (writeGuard_) {
    ::CloseHandle(writeGuard_);
    writeGuard_ = nullptr;
  }
  owner_ = false;
  consumerGeneration_ = 0;
}

HRESULT FrameRing::create(RingNames names) {
  close();

  SECURITY_ATTRIBUTES sa{};
  PSECURITY_DESCRIPTOR sd = nullptr;
  RC_RETURN_IF_FAILED(makeSecurityAttributes(sa, sd));

  const LARGE_INTEGER size = {{static_cast<DWORD>(kSectionBytes & 0xFFFFFFFFull),
                               static_cast<LONG>(kSectionBytes >> 32)}};

  section_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                  static_cast<DWORD>(size.HighPart),
                                  static_cast<DWORD>(size.LowPart), names.section);
  const DWORD createErr = ::GetLastError();
  if (!section_) {
    ::LocalFree(sd);
    const HRESULT hr = HRESULT_FROM_WIN32(createErr);
    if (createErr == ERROR_ACCESS_DENIED) {
      RC_ERR(L"CreateFileMapping(%s) denied. Global\\ sections require "
             L"SeCreateGlobalPrivilege -- this process is neither a service nor elevated.",
             names.section);
    } else {
      RC_ERR(L"CreateFileMapping(%s) failed: %s", names.section, hrMessage(hr).c_str());
    }
    return hr;
  }
  const bool existed = createErr == ERROR_ALREADY_EXISTS;

  event_ = ::CreateEventW(&sa, FALSE /* auto-reset */, FALSE, names.event);
  if (!event_) {
    ::LocalFree(sd);
    const HRESULT hr = RC_HR_FROM_LAST_ERROR();
    RC_ERR(L"CreateEvent(%s) failed: %s", names.event, hrMessage(hr).c_str());
    close();
    return hr;
  }
  writeGuard_ = ::CreateMutexW(&sa, FALSE, names.writeGuard);
  ::LocalFree(sd);
  if (!writeGuard_) {
    const HRESULT hr = RC_HR_FROM_LAST_ERROR();
    RC_ERR(L"CreateMutex(%s) failed: %s", names.writeGuard, hrMessage(hr).c_str());
    close();
    return hr;
  }

  RC_RETURN_IF_FAILED(mapView(true));
  const HRESULT guardHr = lockWriteGuard(writeGuard_);
  if (FAILED(guardHr)) {
    close();
    return guardHr;
  }

  RingHeader* h = headerOf(view_);
  if (!existed) {
    // Begin the C++ object lifetimes explicitly; pagefile-backed zeroes are bytes, not
    // constructed std::atomic objects. magic goes last with release ordering: a reader
    // that observes it with acquire ordering can trust every fixed header field.
    new (h) RingHeader{};
    h->slotCount = kRingSlots;
    h->slotBytes = kRingSlotBytes;
    h->version = kRingVersion;
    h->consumerGeneration.store(1, std::memory_order_relaxed);
    h->consumerCount.store(1, std::memory_order_relaxed);
    h->magic.store(kRingMagic, std::memory_order_release);
    RC_LOG(L"created ring %s (%llu bytes, %u slots)", names.section,
           static_cast<unsigned long long>(kSectionBytes), kRingSlots);
  } else if (h->magic.load(std::memory_order_acquire) != kRingMagic ||
             h->version != kRingVersion ||
             h->slotCount != kRingSlots || h->slotBytes != kRingSlotBytes) {
    // Same four fields open() checks. CreateFileMapping on an existing name returns the
    // existing object and silently ignores both the requested size and the security
    // attributes, so attaching is exactly the case where the layout must be re-verified
    // rather than assumed from our own constants.
    RC_ERR(L"existing ring has magic 0x%08X version %u slots %u; expected 0x%08X/%u/%u",
           h->magic.load(std::memory_order_relaxed), h->version, h->slotCount, kRingMagic,
           kRingVersion, kRingSlots);
    ::ReleaseMutex(writeGuard_);
    close();
    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
  } else {
    const uint32_t count = h->consumerCount.load(std::memory_order_relaxed);
    if (count == 0) {
      uint32_t next =
          h->consumerGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
      if (next == 0) h->consumerGeneration.store(1, std::memory_order_relaxed);
      resetPublishedFrames(h);
      h->consumerCount.store(1, std::memory_order_release);
      RC_LOG(L"reopened ring %s for a new camera-consumer generation", names.section);
    } else {
      if (count == UINT32_MAX) {
        ::ReleaseMutex(writeGuard_);
        close();
        return HRESULT_FROM_WIN32(ERROR_TOO_MANY_OPEN_FILES);
      }
      h->consumerCount.store(count + 1, std::memory_order_release);
    }
    RC_LOG(L"attached to existing ring %s", names.section);
  }

  owner_ = true;
  consumerGeneration_ = h->consumerGeneration.load(std::memory_order_acquire);
  ::ReleaseMutex(writeGuard_);
  return S_OK;
}

HRESULT FrameRing::open(bool writable, RingNames names) {
  close();

  const DWORD access = writable ? (FILE_MAP_READ | FILE_MAP_WRITE) : FILE_MAP_READ;
  section_ = ::OpenFileMappingW(access, FALSE, names.section);
  if (!section_) {
    // ERROR_FILE_NOT_FOUND simply means no application currently has the camera open.
    // That is the normal idle state, so it is logged at debug level and returned to the
    // caller to poll on, not treated as a failure.
    const HRESULT hr = RC_HR_FROM_LAST_ERROR();
    RC_DBG(L"OpenFileMapping(%s) -> %s", names.section, hrMessage(hr).c_str());
    return hr;
  }

  const DWORD eventAccess = writable ? EVENT_MODIFY_STATE : SYNCHRONIZE;
  event_ = ::OpenEventW(eventAccess, FALSE, names.event);
  if (!event_) {
    const HRESULT hr = RC_HR_FROM_LAST_ERROR();
    RC_WARN(L"OpenEvent(%s) -> %s", names.event, hrMessage(hr).c_str());
    close();
    return hr;
  }
  if (writable) {
    writeGuard_ =
        ::OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, names.writeGuard);
    if (!writeGuard_) {
      const HRESULT hr = RC_HR_FROM_LAST_ERROR();
      RC_WARN(L"OpenMutex(%s) -> %s", names.writeGuard, hrMessage(hr).c_str());
      close();
      return hr;
    }
  }

  RC_RETURN_IF_FAILED(mapView(writable));
  if (writable) {
    const HRESULT guardHr = lockWriteGuard(writeGuard_);
    if (FAILED(guardHr)) {
      close();
      return guardHr;
    }
  }

  const RingHeader* h = headerOf(view_);
  if (h->magic.load(std::memory_order_acquire) != kRingMagic ||
      h->version != kRingVersion ||
      h->slotCount != kRingSlots || h->slotBytes != kRingSlotBytes) {
    RC_ERR(L"ring layout mismatch: magic 0x%08X version %u slots %u",
           h->magic.load(std::memory_order_relaxed), h->version, h->slotCount);
    if (writable) ::ReleaseMutex(writeGuard_);
    close();
    return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
  }
  if (!hasActiveConsumer(h)) {
    if (writable) ::ReleaseMutex(writeGuard_);
    close();
    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
  }
  consumerGeneration_ = h->consumerGeneration.load(std::memory_order_acquire);
  if (writable) ::ReleaseMutex(writeGuard_);
  return S_OK;
}

HRESULT FrameRing::mapView(bool writable) {
  const DWORD access = writable ? (FILE_MAP_READ | FILE_MAP_WRITE) : FILE_MAP_READ;
  view_ = ::MapViewOfFile(section_, access, 0, 0, 0);
  if (!view_) {
    const HRESULT hr = RC_HR_FROM_LAST_ERROR();
    RC_ERR(L"MapViewOfFile failed: %s", hrMessage(hr).c_str());
    close();
    return hr;
  }
  return S_OK;
}

HRESULT FrameRing::writeFrame(const uint8_t* src, uint32_t bytes, const FrameInfo& info) {
  RC_RETURN_HR_IF(!valid(), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
  RC_RETURN_IF_NULL(src);
  RC_RETURN_HR_IF(bytes == 0 || bytes > kRingSlotBytes, E_INVALIDARG);

  // Publishing remains non-blocking. The guard is held only during a consumer
  // generation transition or another (unsupported) writer's copy.
  const HRESULT guardHr = lockWriteGuard(writeGuard_, 0);
  if (FAILED(guardHr)) return guardHr;
  RingHeader* h = headerOf(view_);
  if (h->magic.load(std::memory_order_acquire) != kRingMagic ||
      !hasActiveConsumer(h) ||
      h->consumerGeneration.load(std::memory_order_acquire) != consumerGeneration_) {
    ::ReleaseMutex(writeGuard_);
    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
  }
  const uint64_t seq = h->writeSeq.load(std::memory_order_relaxed);
  const uint32_t slot = static_cast<uint32_t>(seq % kRingSlots);

  // Odd sequence marks the slot in flight. Release ordering so a reader that observes
  // the odd value cannot also observe any of the pixel writes that follow it.
  const uint64_t slotSeq = h->slotSeq[slot].load(std::memory_order_relaxed);
  // A producer can die after publishing the odd marker. Keep the next marker odd in
  // that recovery case too; slotSeq + 1 would turn it even before the memcpy and let a
  // reader accept a torn frame from the replacement producer.
  const uint64_t writingSeq = slotSeq + ((slotSeq & 1u) ? 2u : 1u);
  h->slotSeq[slot].store(writingSeq, std::memory_order_release);
  std::atomic_thread_fence(std::memory_order_release);

  std::memcpy(slotOf(view_, slot), src, bytes);
  h->slotPts[slot].store(info.ptsMicros, std::memory_order_relaxed);
  h->slotBytesUsed[slot].store(bytes, std::memory_order_relaxed);
  h->slotWidth[slot].store(info.width, std::memory_order_relaxed);
  h->slotHeight[slot].store(info.height, std::memory_order_relaxed);
  h->slotStride[slot].store(info.stride, std::memory_order_relaxed);

  // If the last consumer closed (and possibly another reopened) during the copy, do
  // not publish this slot into the replacement generation. Leaving it odd is safe; the
  // next write's odd-sequence recovery handles it.
  if (!hasActiveConsumer(h) ||
      h->consumerGeneration.load(std::memory_order_acquire) != consumerGeneration_) {
    ::ReleaseMutex(writeGuard_);
    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
  }

  std::atomic_thread_fence(std::memory_order_release);
  h->slotSeq[slot].store(writingSeq + 1, std::memory_order_release);

  // Geometry is republished every frame rather than only on change. It costs four
  // relaxed stores and removes an entire class of bug where a reader attaches between
  // the change and the next publish and reads stale dimensions.
  if (h->width.load(std::memory_order_relaxed) != info.width ||
      h->height.load(std::memory_order_relaxed) != info.height ||
      h->stride.load(std::memory_order_relaxed) != info.stride ||
      h->format.load(std::memory_order_relaxed) != info.format) {
    h->width.store(info.width, std::memory_order_relaxed);
    h->height.store(info.height, std::memory_order_relaxed);
    h->stride.store(info.stride, std::memory_order_relaxed);
    h->format.store(info.format, std::memory_order_relaxed);
    h->formatGeneration.fetch_add(1, std::memory_order_release);
  }

  h->lastWriteQpc.store(qpcNow(), std::memory_order_relaxed);
  h->writeSeq.store(seq + 1, std::memory_order_release);

  if (event_) ::SetEvent(event_);
  ::ReleaseMutex(writeGuard_);
  return S_OK;
}

HRESULT FrameRing::readLatest(uint8_t* dst, uint32_t dstCapacity, FrameInfo& info) {
  RC_RETURN_HR_IF(!valid(), HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
  RC_RETURN_IF_NULL(dst);

  RingHeader* h = headerOf(view_);

  // Bounded retry. With four slots the writer has to lap the reader completely to
  // invalidate a read twice in a row, so exhausting this means the producer is running
  // far ahead of us and the right answer is to give up on this frame, not to spin.
  for (int attempt = 0; attempt < 8; ++attempt) {
    const uint64_t seq = h->writeSeq.load(std::memory_order_acquire);
    if (seq == 0) return S_FALSE;

    const uint32_t slot = static_cast<uint32_t>((seq - 1) % kRingSlots);
    const uint64_t before = h->slotSeq[slot].load(std::memory_order_acquire);
    if (before & 1u) continue;  // write in flight

    const uint32_t bytes = h->slotBytesUsed[slot].load(std::memory_order_relaxed);
    if (bytes == 0 || bytes > kRingSlotBytes) continue;
    RC_RETURN_HR_IF(bytes > dstCapacity, HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));

    FrameInfo candidate;
    candidate.width = h->slotWidth[slot].load(std::memory_order_relaxed);
    candidate.height = h->slotHeight[slot].load(std::memory_order_relaxed);
    candidate.stride = h->slotStride[slot].load(std::memory_order_relaxed);
    candidate.format = h->format.load(std::memory_order_relaxed);
    candidate.ptsMicros = h->slotPts[slot].load(std::memory_order_relaxed);
    candidate.bytesUsed = bytes;
    candidate.writeSeq = seq;

    // Checked before the copy, so a nonsensical frame costs nothing and -- more to the
    // point -- is never handed to a caller that would act on its stride.
    if (!geometryIsSane(candidate)) {
      // Logged on the transition only. A producer stuck publishing bad geometry does so
      // at frame rate, and thirty identical lines a second would bury the surrounding
      // context and churn through the log's size cap in minutes.
      if (!rejectedLogged_) {
        rejectedLogged_ = true;
        RC_WARN(L"rejecting frames with implausible geometry: %ux%u stride %u, %u bytes",
                candidate.width, candidate.height, candidate.stride, candidate.bytesUsed);
      }
      return S_FALSE;
    }
    if (rejectedLogged_) {
      rejectedLogged_ = false;
      RC_LOG(L"frame geometry is valid again (%ux%u stride %u)", candidate.width,
             candidate.height, candidate.stride);
    }

    std::memcpy(dst, slotOf(view_, slot), bytes);

    std::atomic_thread_fence(std::memory_order_acquire);
    if (h->slotSeq[slot].load(std::memory_order_acquire) != before) continue;  // torn

    info = candidate;
    return S_OK;
  }

  RC_DBG(L"readLatest gave up after 8 contended attempts");
  return S_FALSE;
}

uint64_t FrameRing::millisSinceLastWrite() const {
  if (!view_) return UINT64_MAX;
  const RingHeader* h = headerOf(view_);
  if (h->writeSeq.load(std::memory_order_acquire) == 0) return UINT64_MAX;

  const uint64_t last = h->lastWriteQpc.load(std::memory_order_relaxed);
  const uint64_t now = qpcNow();

  // A timestamp in the future is not possible from a correct producer, and this field
  // lives in memory any interactive user can write. Treating it as "just published"
  // would let a bogus value pin the source permanently on a stale frame, suppressing
  // the placeholder that is supposed to appear when a producer dies. Fail towards
  // stale: it is the state that degrades safely.
  if (now < last) return UINT64_MAX;
  return (now - last) * 1000ull / qpcFrequency();
}

}  // namespace rcwin
