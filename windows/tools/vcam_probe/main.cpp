// rc-vcam-probe.exe -- opens the virtual camera and proves it actually delivers frames.
//
// Unit tests cannot reach this component: it only exists once Windows has loaded our
// DLL into the Frame Server. So this tool is the automated half of M1's verification,
// and it deliberately checks the camera through BOTH stacks.
//
// Media Foundation and DirectShow matter independently. A single MF virtual camera is
// supposed to be visible to both, which is what lets one implementation cover Zoom,
// Teams, Discord, Chrome, OBS and the Windows Camera app -- but "supposed to" is not
// evidence, and the two paths negotiate formats through completely different code.
//
// The assertions come from the structure of the test pattern (see rcwin/test_pattern.h):
//
//   static region  identical hash on every frame  -> stride, plane offsets and colour
//                                                    handling are all correct
//   moving region  different hash on every frame  -> the camera is live, not repeating
//                                                    one frame forever
//
// Those two failures look identical in a preview window and have nothing to do with
// each other, which is the whole reason they are separated here.

#include <windows.h>

#include <dshow.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include "rcwin/guids.h"
#include "rcwin/hr.h"
#include "rcwin/nv12.h"
#include "rcwin/test_pattern.h"

using Microsoft::WRL::ComPtr;

// --- qedit.h replacements ---------------------------------------------------
//
// qedit.h was removed from the Windows SDK years ago, but CLSID_SampleGrabber and
// CLSID_NullRenderer are still registered in Windows 11 and still work. Redeclaring
// the two interfaces locally is the standard workaround; the vtable layout and IIDs
// are fixed by COM and cannot change without breaking every existing binary.

interface ISampleGrabberCB : public IUnknown {
  virtual STDMETHODIMP SampleCB(double SampleTime, IMediaSample* pSample) = 0;
  virtual STDMETHODIMP BufferCB(double SampleTime, BYTE* pBuffer, long BufferLen) = 0;
};

interface ISampleGrabber : public IUnknown {
  virtual STDMETHODIMP SetOneShot(BOOL OneShot) = 0;
  virtual STDMETHODIMP SetMediaType(const AM_MEDIA_TYPE* pType) = 0;
  virtual STDMETHODIMP GetConnectedMediaType(AM_MEDIA_TYPE* pType) = 0;
  virtual STDMETHODIMP SetBufferSamples(BOOL BufferThem) = 0;
  virtual STDMETHODIMP GetCurrentBuffer(long* pBufferSize, long* pBuffer) = 0;
  virtual STDMETHODIMP GetCurrentSample(IMediaSample** ppSample) = 0;
  virtual STDMETHODIMP SetCallback(ISampleGrabberCB* pCallback, long WhichMethodToCallback) = 0;
};

static const GUID CLSID_SampleGrabber_ = {
    0xC1F400A0, 0x3F08, 0x11D3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
static const GUID CLSID_NullRenderer_ = {
    0xC1F400A4, 0x3F08, 0x11D3, {0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37}};
static const GUID IID_ISampleGrabber_ = {
    0x6B652FFF, 0x11FE, 0x4FCE, {0x92, 0xAD, 0x02, 0x66, 0xB5, 0xD7, 0xC7, 0x8F}};
static const GUID IID_ISampleGrabberCB_ = {
    0x0579154A, 0x2B53, 0x4994, {0xB0, 0xD0, 0xE7, 0x73, 0x14, 0x8E, 0xFF, 0x85}};

namespace {

// --- shared analysis --------------------------------------------------------

struct FrameStat {
  uint64_t staticHash = 0;
  uint64_t movingHash = 0;
  long long timestamp100ns = 0;
};

struct ProbeFormat {
  UINT32 width = 0;
  UINT32 height = 0;
  UINT32 fps = 0;

  bool specified() const { return width != 0; }
};

using FormatKey = std::tuple<UINT32, UINT32, UINT32>;

constexpr std::array<ProbeFormat, 12> kExpectedLadder = {{
    {1920, 1080, 30}, {1920, 1080, 60}, {3840, 2160, 30},
    {3840, 2160, 60}, {2560, 1440, 30}, {2560, 1440, 60},
    {1280, 720, 30},  {1280, 720, 60},  {960, 540, 30},
    {960, 540, 60},   {640, 480, 30},   {640, 480, 60},
}};

// MF_SOURCE_READER_FIRST_VIDEO_STREAM is an enumerator whose value (0xFFFFFFFC) does
// not fit the signed enum type the SDK gives it, so passing it directly to a DWORD
// parameter warns at /W4. Converted once here rather than casting at each call site.
constexpr DWORD kFirstVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

uint64_t fnv1a(const uint8_t* data, size_t n, uint64_t hash) {
  for (size_t i = 0; i < n; ++i) {
    hash ^= data[i];
    hash *= 0x100000001b3ull;
  }
  return hash;
}

// Hashes only the visible pixels of each row. Stride padding is never displayed and is
// not guaranteed to be initialised by the buffer's owner, so including it would make
// the "static region never changes" assertion fail for reasons that do not matter.
uint64_t hashLumaRows(const uint8_t* nv12, int stride, int width, int top, int height) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (int row = top; row < top + height; ++row) {
    hash = fnv1a(nv12 + static_cast<size_t>(row) * stride, static_cast<size_t>(width), hash);
  }
  return hash;
}

void analyse(const uint8_t* nv12, int stride, int width, int height, FrameStat& out) {
  const rcwin::Nv12Layout layout = rcwin::nv12Layout(width, height, stride);
  const rcwin::PatternRegions r = rcwin::patternRegions(layout);
  out.staticHash = hashLumaRows(nv12, stride, width, r.staticTop, r.staticHeight);
  out.movingHash = hashLumaRows(nv12, stride, width, r.movingTop, r.movingHeight);
}

// The verdict. Both properties are reported even when one fails, because knowing which
// of the two broke is most of the diagnosis.
int report(const wchar_t* label, const std::vector<FrameStat>& frames, bool regionsValid,
           UINT32 expectedFps) {
  std::wprintf(L"\n--- %s: %zu frames ---\n", label, frames.size());
  if (frames.empty()) {
    std::wprintf(L"FAIL: no frames delivered\n");
    return 1;
  }

  double meanMs = 0.0;
  double maxJitterMs = 0.0;
  if (frames.size() > 1) {
    std::vector<double> deltas;
    deltas.reserve(frames.size() - 1);
    for (size_t i = 1; i < frames.size(); ++i) {
      deltas.push_back((frames[i].timestamp100ns - frames[i - 1].timestamp100ns) / 10000.0);
    }
    for (double d : deltas) meanMs += d;
    meanMs /= static_cast<double>(deltas.size());
    for (double d : deltas) maxJitterMs = (std::max)(maxJitterMs, std::abs(d - meanMs));
  }
  std::wprintf(L"  mean interval : %.2f ms  (%.1f fps)\n", meanMs,
               meanMs > 0.0 ? 1000.0 / meanMs : 0.0);
  std::wprintf(L"  max jitter    : %.2f ms\n", maxJitterMs);

  bool pacingValid = true;
  if (expectedFps != 0 && frames.size() > 1) {
    const double expectedMs = 1000.0 / static_cast<double>(expectedFps);
    const double toleranceMs = (std::max)(2.0, expectedMs * 0.15);
    pacingValid = std::abs(meanMs - expectedMs) <= toleranceMs;
    std::wprintf(L"  pacing        : %s (expected %.2f ms at %u fps)\n",
                 pacingValid ? L"PASS" : L"FAIL", expectedMs, expectedFps);
  }

  if (!regionsValid) {
    // Region hashing only means something when the frames really are NV12 in our
    // geometry. Saying so beats printing a confident PASS derived from bytes we did
    // not understand.
    std::wprintf(L"  region checks : SKIPPED (format is not the expected NV12 geometry)\n");
    std::wprintf(L"%s: frames delivered, format unverified\n", label);
    return pacingValid ? 0 : 1;
  }

  size_t staticChanges = 0;
  size_t movingRepeats = 0;
  for (size_t i = 1; i < frames.size(); ++i) {
    if (frames[i].staticHash != frames[0].staticHash) ++staticChanges;
    if (frames[i].movingHash == frames[i - 1].movingHash) ++movingRepeats;
  }

  std::wprintf(L"  static region : %s (0x%016llX, %zu of %zu frames differ)\n",
               staticChanges == 0 ? L"STABLE  PASS" : L"CHANGED  FAIL",
               static_cast<unsigned long long>(frames[0].staticHash), staticChanges,
               frames.size() - 1);
  std::wprintf(L"  moving region : %s (%zu of %zu consecutive pairs repeat)\n",
               movingRepeats == 0 ? L"ADVANCING  PASS" : L"REPEATED  FAIL", movingRepeats,
               frames.size() - 1);

  const bool ok = staticChanges == 0 && movingRepeats == 0 && pacingValid;
  std::wprintf(L"%s: %s\n", label, ok ? L"PASS" : L"FAIL");
  return ok ? 0 : 1;
}

bool parseProbeFormat(const wchar_t* text, ProbeFormat& out) {
  if (text == nullptr) return false;
  wchar_t* end = nullptr;
  const unsigned long width = std::wcstoul(text, &end, 10);
  if (end == text || (*end != L'x' && *end != L'X')) return false;
  const wchar_t* heightText = end + 1;
  const unsigned long height = std::wcstoul(heightText, &end, 10);
  if (end == heightText || *end != L'@') return false;
  const wchar_t* fpsText = end + 1;
  const unsigned long fps = std::wcstoul(fpsText, &end, 10);
  if (end == fpsText || *end != L'\0' || width > UINT32_MAX || height > UINT32_MAX ||
      fps > UINT32_MAX) {
    return false;
  }
  const ProbeFormat parsed{static_cast<UINT32>(width), static_cast<UINT32>(height),
                           static_cast<UINT32>(fps)};
  for (const ProbeFormat& expected : kExpectedLadder) {
    if (expected.width == parsed.width && expected.height == parsed.height &&
        expected.fps == parsed.fps) {
      out = parsed;
      return true;
    }
  }
  return false;
}

void writeRaw(const std::wstring& dir, size_t index, const uint8_t* data, size_t bytes) {
  if (dir.empty()) return;
  wchar_t path[MAX_PATH];
  swprintf_s(path, L"%s\\frame_%03zu.nv12", dir.c_str(), index);
  HANDLE h = ::CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return;
  DWORD written = 0;
  ::WriteFile(h, data, static_cast<DWORD>(bytes), &written, nullptr);
  ::CloseHandle(h);
}

// --- Media Foundation path --------------------------------------------------

HRESULT findMfDevice(const std::wstring& name, IMFActivate** out) {
  *out = nullptr;
  ComPtr<IMFAttributes> attributes;
  RC_RETURN_IF_FAILED(::MFCreateAttributes(&attributes, 1));
  RC_RETURN_IF_FAILED(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                          MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));

  IMFActivate** devices = nullptr;
  UINT32 count = 0;
  RC_RETURN_IF_FAILED(::MFEnumDeviceSources(attributes.Get(), &devices, &count));

  HRESULT hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  for (UINT32 i = 0; i < count; ++i) {
    WCHAR* friendly = nullptr;
    UINT32 length = 0;
    if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                                                 &friendly, &length)) &&
        friendly != nullptr) {
      if (*out == nullptr && name == friendly) {
        *out = devices[i];
        (*out)->AddRef();
        hr = S_OK;
      }
      ::CoTaskMemFree(friendly);
    }
    devices[i]->Release();
  }
  ::CoTaskMemFree(devices);
  return hr;
}

int verifyMfLadder(IMFSourceReader* reader, const ProbeFormat& requested,
                   IMFMediaType** selectedType) {
  if (reader == nullptr || selectedType == nullptr) return 1;
  *selectedType = nullptr;

  std::set<FormatKey> actual;
  ComPtr<IMFMediaType> requestedType;
  for (DWORD index = 0;; ++index) {
    ComPtr<IMFMediaType> type;
    const HRESULT hr = reader->GetNativeMediaType(kFirstVideoStream, index, &type);
    if (hr == MF_E_NO_MORE_TYPES) break;
    if (FAILED(hr)) {
      std::wprintf(L"MF: native media-type enumeration failed at %u: %s\n", index,
                   rcwin::hrMessage(hr).c_str());
      return 1;
    }

    GUID subtype = GUID_NULL;
    UINT32 width = 0, height = 0, numerator = 0, denominator = 0;
    if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) ||
        subtype != MFVideoFormat_NV12 ||
        FAILED(::MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height)) ||
        FAILED(::MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &numerator,
                                     &denominator)) ||
        denominator == 0 || numerator % denominator != 0) {
      continue;
    }

    const UINT32 fps = numerator / denominator;
    actual.emplace(width, height, fps);
    if (requested.specified() && width == requested.width && height == requested.height &&
        fps == requested.fps) {
      requestedType = type;
    }
  }

  bool complete = true;
  for (const ProbeFormat& expected : kExpectedLadder) {
    if (!actual.contains({expected.width, expected.height, expected.fps})) {
      complete = false;
      std::wprintf(L"MF: missing native NV12 type %ux%u@%u\n", expected.width,
                   expected.height, expected.fps);
    }
  }
  std::wprintf(L"MF: NV12 format ladder %s (%zu expected types visible)\n",
               complete ? L"PASS" : L"FAIL", actual.size());

  if (requested.specified() && !requestedType) {
    std::wprintf(L"MF: requested type %ux%u@%u is not available\n", requested.width,
                 requested.height, requested.fps);
    return 1;
  }
  if (requestedType) requestedType.CopyTo(selectedType);
  return complete ? 0 : 1;
}

int runMf(const std::wstring& name, int wanted, const std::wstring& outDir,
          const ProbeFormat& requested) {
  ComPtr<IMFActivate> activate;
  HRESULT hr = findMfDevice(name, &activate);
  if (FAILED(hr)) {
    std::wprintf(L"MF: device \"%s\" not found (%s)\n", name.c_str(),
                 rcwin::hrMessage(hr).c_str());
    return 1;
  }

  ComPtr<IMFMediaSource> source;
  hr = activate->ActivateObject(IID_PPV_ARGS(&source));
  if (FAILED(hr)) {
    std::wprintf(L"MF: ActivateObject failed: %s\n", rcwin::hrMessage(hr).c_str());
    return 1;
  }

  ComPtr<IMFSourceReader> reader;
  hr = ::MFCreateSourceReaderFromMediaSource(source.Get(), nullptr, &reader);
  if (FAILED(hr)) {
    std::wprintf(L"MF: MFCreateSourceReaderFromMediaSource failed: %s\n",
                 rcwin::hrMessage(hr).c_str());
    source->Shutdown();
    return 1;
  }

  ComPtr<IMFMediaType> requestedType;
  const int ladderResult = verifyMfLadder(reader.Get(), requested, &requestedType);
  if (requested.specified() && !requestedType) {
    source->Shutdown();
    activate->ShutdownObject();
    return 1;
  }
  if (requestedType) {
    hr = reader->SetCurrentMediaType(kFirstVideoStream, nullptr, requestedType.Get());
    if (FAILED(hr)) {
      std::wprintf(L"MF: selecting %ux%u@%u failed: %s\n", requested.width,
                   requested.height, requested.fps, rcwin::hrMessage(hr).c_str());
      source->Shutdown();
      activate->ShutdownObject();
      return 1;
    }
  }

  ComPtr<IMFMediaType> type;
  UINT32 width = 0, height = 0;
  UINT32 fpsNumerator = 0, fpsDenominator = 0;
  GUID subtype = GUID_NULL;
  if (SUCCEEDED(reader->GetCurrentMediaType(kFirstVideoStream, &type))) {
    ::MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
    ::MFGetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, &fpsNumerator, &fpsDenominator);
    type->GetGUID(MF_MT_SUBTYPE, &subtype);
  }
  const bool isNv12 = subtype == MFVideoFormat_NV12;
  const UINT32 activeFps = fpsDenominator == 0 ? 0 : fpsNumerator / fpsDenominator;
  std::wprintf(L"MF: %ux%u@%u, subtype %s\n", width, height, activeFps,
               isNv12 ? L"NV12" : L"(not NV12)");

  std::vector<FrameStat> frames;
  frames.reserve(static_cast<size_t>(wanted));

  // Bounded on both attempts and wall clock. ReadSample legitimately returns a null
  // sample for a stream tick, so "keep going until we have enough frames" spins
  // forever against a source that ticks but never delivers -- which is precisely the
  // failure this tool is most likely to be pointed at. A diagnostic that hangs on the
  // fault it exists to diagnose is worse than useless.
  const ULONGLONG deadline = ::GetTickCount64() + 10000 + static_cast<ULONGLONG>(wanted) * 100;
  int emptyReads = 0;

  for (int i = 0; i < wanted;) {
    if (::GetTickCount64() > deadline) {
      std::wprintf(L"MF: timed out with %zu of %d frames\n", frames.size(), wanted);
      break;
    }

    DWORD streamIndex = 0, flags = 0;
    LONGLONG timestamp = 0;
    ComPtr<IMFSample> sample;
    hr = reader->ReadSample(kFirstVideoStream, 0, &streamIndex, &flags, &timestamp, &sample);
    if (FAILED(hr)) {
      std::wprintf(L"MF: ReadSample failed: %s\n", rcwin::hrMessage(hr).c_str());
      break;
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
    if (!sample) {
      // A stream tick carries no data. Counted so a source that only ticks is reported
      // as such instead of pinning a core until the deadline.
      if (++emptyReads > 1000) {
        std::wprintf(L"MF: 1000 consecutive stream ticks with no sample; giving up\n");
        break;
      }
      continue;
    }
    emptyReads = 0;

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) continue;

    BYTE* data = nullptr;
    DWORD maxLength = 0, currentLength = 0;
    if (FAILED(buffer->Lock(&data, &maxLength, &currentLength))) continue;

    FrameStat stat;
    stat.timestamp100ns = timestamp;
    if (isNv12 && width > 0 && height > 0) {
      // A contiguous buffer is packed, so the stride is the width by definition.
      analyse(data, static_cast<int>(width), static_cast<int>(width), static_cast<int>(height),
              stat);
    }
    writeRaw(outDir, frames.size(), data, currentLength);
    buffer->Unlock();

    frames.push_back(stat);
    ++i;
  }

  source->Shutdown();
  activate->ShutdownObject();
  return ladderResult |
         report(L"Media Foundation", frames, isNv12 && width > 0, activeFps);
}

// --- DirectShow path --------------------------------------------------------

class GrabberCallback final : public ISampleGrabberCB {
 public:
  GrabberCallback(int wanted, int width, int height, bool nv12, std::wstring outDir)
      : wanted_(wanted), width_(width), height_(height), nv12_(nv12),
        outDir_(std::move(outDir)) {
    done_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }
  ~GrabberCallback() {
    if (done_) ::CloseHandle(done_);
  }

  // Lifetime is owned by the caller's stack frame; the graph never outlives it, so the
  // refcount here is deliberately inert rather than a second owner.
  IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    // Compared against a local IID rather than __uuidof: the interface is redeclared
    // here without a DECLSPEC_UUID, so there is no compiler-known GUID to ask for.
    if (riid == IID_IUnknown || riid == IID_ISampleGrabberCB_) {
      *ppv = static_cast<ISampleGrabberCB*>(this);
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  IFACEMETHODIMP_(ULONG) AddRef() override { return 2; }
  IFACEMETHODIMP_(ULONG) Release() override { return 1; }

  IFACEMETHODIMP SampleCB(double, IMediaSample*) override { return E_NOTIMPL; }

  IFACEMETHODIMP BufferCB(double sampleTime, BYTE* buffer, long length) override {
    if (static_cast<int>(frames_.size()) >= wanted_) return S_OK;

    FrameStat stat;
    // DirectShow reports seconds as a double; the rest of this tool works in 100 ns
    // units, so convert here rather than special-casing the report.
    stat.timestamp100ns = static_cast<long long>(sampleTime * 10000000.0);
    // The length is checked against the geometry the graph reported rather than
    // assumed to agree with it. analyse() indexes rows by that geometry, so a short
    // buffer from a misdescribing filter would read past the end of somebody else's
    // allocation -- and this tool exists to diagnose faults, not to add one.
    const long needed = static_cast<long>(width_) * height_ * 3 / 2;
    if (nv12_ && width_ > 0 && height_ > 0 && length >= needed) {
      analyse(buffer, width_, width_, height_, stat);
    }
    writeRaw(outDir_, frames_.size(), buffer, static_cast<size_t>(length));
    frames_.push_back(stat);

    if (static_cast<int>(frames_.size()) >= wanted_) ::SetEvent(done_);
    return S_OK;
  }

  HANDLE doneEvent() const { return done_; }
  const std::vector<FrameStat>& frames() const { return frames_; }

 private:
  int wanted_;
  int width_;
  int height_;
  bool nv12_;
  std::wstring outDir_;
  std::vector<FrameStat> frames_;
  HANDLE done_ = nullptr;
};

HRESULT findDshowFilter(const std::wstring& name, IBaseFilter** out) {
  if (out == nullptr) return E_POINTER;
  *out = nullptr;
  ComPtr<ICreateDevEnum> devEnum;
  RC_RETURN_IF_FAILED(::CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(&devEnum)));

  ComPtr<IEnumMoniker> monikers;
  const HRESULT hr =
      devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &monikers, 0);
  // S_FALSE means the category exists but is empty -- no cameras at all.
  if (hr != S_OK) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
  if (!monikers) return E_UNEXPECTED;

  ComPtr<IMoniker> moniker;
  ComPtr<IBindCtx> bindContext;
  RC_RETURN_IF_FAILED(::CreateBindCtx(0, &bindContext));
  while (monikers->Next(1, &moniker, nullptr) == S_OK) {
    if (!moniker) continue;
    ComPtr<IPropertyBag> bag;
    if (SUCCEEDED(moniker->BindToStorage(bindContext.Get(), nullptr, IID_PPV_ARGS(&bag))) && bag) {
      VARIANT var;
      ::VariantInit(&var);
      if (SUCCEEDED(bag->Read(L"FriendlyName", &var, nullptr))) {
        const bool match = var.bstrVal && name == var.bstrVal;
        ::VariantClear(&var);
        if (match) {
          const HRESULT bind =
              moniker->BindToObject(bindContext.Get(), nullptr, IID_PPV_ARGS(out));
          if (SUCCEEDED(bind)) return S_OK;
          return bind;
        }
      }
    }
    moniker.Reset();
  }
  return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

int runDshow(const std::wstring& name, int wanted, const std::wstring& outDir) {
  ComPtr<IBaseFilter> sourceFilter;
  HRESULT hr = findDshowFilter(name, &sourceFilter);
  if (FAILED(hr)) {
    std::wprintf(L"DirectShow: device \"%s\" not found (%s)\n", name.c_str(),
                 rcwin::hrMessage(hr).c_str());
    return 1;
  }

  ComPtr<IGraphBuilder> graph;
  RC_RETURN_IF_FAILED(::CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(&graph)));
  ComPtr<ICaptureGraphBuilder2> builder;
  RC_RETURN_IF_FAILED(::CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr,
                                         CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&builder)));
  RC_RETURN_IF_FAILED(builder->SetFiltergraph(graph.Get()));

  ComPtr<IBaseFilter> grabberFilter;
  RC_RETURN_IF_FAILED(::CoCreateInstance(CLSID_SampleGrabber_, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(&grabberFilter)));
  ComPtr<ISampleGrabber> grabber;
  RC_RETURN_IF_FAILED(
      grabberFilter->QueryInterface(IID_ISampleGrabber_, reinterpret_cast<void**>(
                                                             grabber.GetAddressOf())));

  // Ask for NV12 specifically. If the DirectShow wrapper around our MF source will not
  // give us NV12 the graph still renders through whatever it does offer, and the report
  // says the region checks were skipped rather than pretending they passed.
  AM_MEDIA_TYPE wantType{};
  wantType.majortype = MEDIATYPE_Video;
  wantType.subtype = MEDIASUBTYPE_NV12;
  wantType.formattype = GUID_NULL;
  RC_RETURN_IF_FAILED(grabber->SetMediaType(&wantType));
  RC_RETURN_IF_FAILED(grabber->SetBufferSamples(FALSE));

  ComPtr<IBaseFilter> nullRenderer;
  RC_RETURN_IF_FAILED(::CoCreateInstance(CLSID_NullRenderer_, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(&nullRenderer)));

  RC_RETURN_IF_FAILED(graph->AddFilter(sourceFilter.Get(), L"RemoteCam"));
  RC_RETURN_IF_FAILED(graph->AddFilter(grabberFilter.Get(), L"Grabber"));
  RC_RETURN_IF_FAILED(graph->AddFilter(nullRenderer.Get(), L"Null"));

  hr = builder->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, sourceFilter.Get(),
                             grabberFilter.Get(), nullRenderer.Get());
  if (FAILED(hr)) {
    std::wprintf(L"DirectShow: RenderStream failed: %s\n", rcwin::hrMessage(hr).c_str());
    return 1;
  }

  AM_MEDIA_TYPE connected{};
  int width = 0, height = 0;
  UINT32 activeFps = 0;
  bool nv12 = false;
  if (SUCCEEDED(grabber->GetConnectedMediaType(&connected))) {
    nv12 = connected.subtype == MEDIASUBTYPE_NV12;
    if (connected.formattype == FORMAT_VideoInfo && connected.pbFormat &&
        connected.cbFormat >= sizeof(VIDEOINFOHEADER)) {
      const auto* vih = reinterpret_cast<VIDEOINFOHEADER*>(connected.pbFormat);
      width = vih->bmiHeader.biWidth;
      height = std::abs(vih->bmiHeader.biHeight);
      if (vih->AvgTimePerFrame > 0) {
        activeFps = static_cast<UINT32>(
            std::llround(10000000.0 / static_cast<double>(vih->AvgTimePerFrame)));
      }
    }
    if (connected.pbFormat) ::CoTaskMemFree(connected.pbFormat);
    if (connected.pUnk) connected.pUnk->Release();
  }
  std::wprintf(L"DirectShow: %dx%d@%u, subtype %s\n", width, height, activeFps,
               nv12 ? L"NV12" : L"(other)");

  GrabberCallback callback(wanted, width, height, nv12, outDir);
  RC_RETURN_IF_FAILED(grabber->SetCallback(&callback, 1 /* BufferCB */));

  ComPtr<IMediaControl> control;
  RC_RETURN_IF_FAILED(graph->QueryInterface(IID_PPV_ARGS(&control)));
  RC_RETURN_IF_FAILED(control->Run());

  // Generous but finite: at 30 fps, 60 frames need 2 s. Ten seconds of headroom
  // distinguishes "slow" from "never", which a wait of INFINITE would not.
  const DWORD timeoutMs = static_cast<DWORD>(10000 + wanted * 100);
  const DWORD wait = ::WaitForSingleObject(callback.doneEvent(), timeoutMs);
  control->Stop();
  grabber->SetCallback(nullptr, 0);

  if (wait == WAIT_TIMEOUT) {
    std::wprintf(L"DirectShow: timed out after %u ms\n", timeoutMs);
  }
  return report(L"DirectShow", callback.frames(), nv12 && width > 0, activeFps);
}

// --- enumeration ------------------------------------------------------------

void listDevices() {
  std::wprintf(L"Media Foundation video capture devices:\n");
  ComPtr<IMFAttributes> attributes;
  if (SUCCEEDED(::MFCreateAttributes(&attributes, 1)) &&
      SUCCEEDED(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                                    MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) {
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    if (SUCCEEDED(::MFEnumDeviceSources(attributes.Get(), &devices, &count))) {
      if (count == 0) std::wprintf(L"  (none)\n");
      for (UINT32 i = 0; i < count; ++i) {
        WCHAR* friendly = nullptr;
        UINT32 length = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
                                                     &friendly, &length)) &&
            friendly != nullptr) {
          std::wprintf(L"  %s\n", friendly);
          ::CoTaskMemFree(friendly);
        }
        devices[i]->Release();
      }
      ::CoTaskMemFree(devices);
    }
  }

  std::wprintf(L"\nDirectShow video input devices:\n");
  ComPtr<ICreateDevEnum> devEnum;
  if (FAILED(::CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&devEnum)))) {
    return;
  }
  ComPtr<IEnumMoniker> monikers;
  if (devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &monikers, 0) != S_OK) {
    std::wprintf(L"  (none)\n");
    return;
  }
  if (!monikers) return;
  ComPtr<IBindCtx> bindContext;
  if (FAILED(::CreateBindCtx(0, &bindContext)) || !bindContext) return;
  ComPtr<IMoniker> moniker;
  while (monikers->Next(1, &moniker, nullptr) == S_OK) {
    if (!moniker) continue;
    ComPtr<IPropertyBag> bag;
    if (SUCCEEDED(moniker->BindToStorage(bindContext.Get(), nullptr, IID_PPV_ARGS(&bag))) && bag) {
      VARIANT var;
      ::VariantInit(&var);
      if (SUCCEEDED(bag->Read(L"FriendlyName", &var, nullptr))) {
        if (var.bstrVal != nullptr) std::wprintf(L"  %s\n", var.bstrVal);
        ::VariantClear(&var);
      }
    }
    moniker.Reset();
  }
}

int usage() {
  std::wprintf(
      L"rc-vcam-probe -- pull frames from the RemoteCam virtual camera\n"
      L"\n"
      L"  --list                enumerate cameras through both MF and DirectShow\n"
      L"  --mf                  capture through Media Foundation\n"
      L"  --directshow          capture through DirectShow\n"
      L"  --frames N            how many frames to capture (default 60)\n"
      L"  --format WxH@FPS       select an exact MF native type (for example 1280x720@60)\n"
      L"  --name NAME           device friendly name (default \"RemoteCam\")\n"
      L"  --out DIR             also write each frame as raw .nv12\n"
      L"\n"
      L"Exit code is 0 only if every requested path passed.\n"
      L"\n");
  return 2;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  rcwin::logInit(L"rc-vcam-probe");

  bool doList = false, doMf = false, doDshow = false;
  int frames = 60;
  std::wstring name = rcwin::kFriendlyName;
  std::wstring outDir;
  ProbeFormat requestedFormat;

  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i];
    const bool hasValue = i + 1 < argc;
    if (arg == L"--list") {
      doList = true;
    } else if (arg == L"--mf") {
      doMf = true;
    } else if (arg == L"--directshow" || arg == L"--dshow") {
      doDshow = true;
    } else if (arg == L"--frames" && hasValue) {
      frames = _wtoi(argv[++i]);
    } else if (arg == L"--format" && hasValue) {
      if (!parseProbeFormat(argv[++i], requestedFormat)) {
        std::wprintf(L"Invalid format. Expected one of the supported WxH@30/60 types.\n\n");
        return usage();
      }
    } else if (arg == L"--name" && hasValue) {
      name = argv[++i];
    } else if (arg == L"--out" && hasValue) {
      outDir = argv[++i];
      ::CreateDirectoryW(outDir.c_str(), nullptr);
    } else {
      return usage();
    }
  }
  if (!doList && !doMf && !doDshow) return usage();
  if (requestedFormat.specified() && !doMf) {
    std::wprintf(L"--format applies to the Media Foundation path; add --mf.\n\n");
    return usage();
  }
  if (frames < 2) frames = 2;

  HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(hr)) {
    std::wprintf(L"CoInitializeEx failed: %s\n", rcwin::hrMessage(hr).c_str());
    return 1;
  }
  hr = ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
  if (FAILED(hr)) {
    std::wprintf(L"MFStartup failed: %s\n", rcwin::hrMessage(hr).c_str());
    ::CoUninitialize();
    return 1;
  }

  int exitCode = 0;
  if (doList) listDevices();
  if (doMf) exitCode |= runMf(name, frames, outDir, requestedFormat);
  if (doDshow) exitCode |= runDshow(name, frames, outDir);

  ::MFShutdown();
  ::CoUninitialize();
  return exitCode;
}
