# RemoteCam — Open-Source iVCam Replacement

> **Status — 2026-07-28.** `core/` transform math is implemented and verified
> (12,101 assertions passing, under both GCC and MSVC). **M1 is written and building**
> — the virtual camera DLL, its registration helper, a two-stack probe, the `Global\`
> frame ring and a stand-in producer, with 111 more assertions passing — but has not
> yet been verified against a live camera. See
> [Status and handoff](#status-and-handoff) at the end of this document for who owns
> what and where to pick up. Agent operating guide is in [CLAUDE.md](CLAUDE.md).
>
> **Three errors in the original plan have been corrected in place** — the fit modes
> in §2, the pan clamping they imply, and who creates the shared-memory section in §1.
> All three are described in [Corrections](#corrections-to-the-original-plan).

## Context

iVCam (e2eSoft) is the de-facto "phone as PC webcam" tool on Windows, and it is showing its age:

- **It's paywalled.** Free tier gets a burned-in watermark, ads, and drops to 640×480 after the trial. $7.99–$12.99/yr or $24.99 permanent. Manual camera controls (ISO, exposure, white balance, focus) are gated behind the *Premium* tier specifically.
- **The PC can only flip the image, not rotate it.** If you mount the phone sideways or overhead — the single most common reason to use a phone as a webcam — you cannot correct the framing on the PC side.
- **No GPU acceleration story.** Its background removal is CPU/phone-side. It does nothing with the RTX silicon most streamers already own.
- **No encryption, no pairing.** Anything on your LAN can attempt to connect to your camera.
- **iOS can't stream in the background.** Their own docs say background operation is Android-only.
- Closed source, Windows-only client.

RemoteCam is a from-scratch open-source replacement: full iVCam feature parity, zero monetization (no watermark, no ads, no tiers, nothing gated), **arbitrary-angle rotation with crop/pan/zoom** as a first-class feature, and **NVIDIA Maxine** (the SDKs behind NVIDIA Broadcast) wired directly into the frame pipeline for users with RTX GPUs — with an ONNX/DirectML fallback so AMD and Intel users get the same features.

Intended outcome: a Windows 11 app + iPhone app that a streamer, remote worker, or maker can install and use for free, that handles a sideways-mounted phone correctly, and that is measurably lower-latency and better-looking than the incumbent.

## Decisions already made

| Decision | Choice | Consequence |
|---|---|---|
| Windows floor | **Windows 11 only** (build 22000+) | `MFCreateVirtualCamera` only. No DirectShow filter, no driver signing. Sink is behind an interface so a DirectShow backend can be added later. |
| Audio | **Out of scope for v1** | No virtual mic driver, no signing costs. Protocol reserves an audio channel so adding it later is not a breaking change. |
| Windows stack | **C++20 core + Qt 6 / QML** | One language end to end; QRhi already uses D3D11 on Windows, so preview is zero-copy. |
| iOS distribution | **App Store-grade entitlements, sideloading documented** | Build assumes a paid dev account for the public release; repo stays self-signable from source. |
| Non-NVIDIA users | **ONNX Runtime + DirectML fallback** | Same effects UI for everyone; NVIDIA path is faster/better, not exclusive. |
| Security | **Pairing required, encryption optional** | New devices must be approved on the PC. Control channel always authenticated; media encryption is a default-off toggle. |
| v1 extras | Background capture on iOS, PC record + screenshot, OBS source plugin | Multi-phone → multi-camera deferred to post-v1 (architecture keeps the door open). |

## Feature matrix

**iVCam parity (all free, nothing gated):**
Wi-Fi auto-discovery + manual IP + recent connections · USB · up to 4K60 · H.264 + HEVC · front/back + ultra-wide/wide/telephoto with seamless transitions · pinch zoom · tap-to-focus · AE/AF/AWB lock · manual ISO / shutter / EV / white balance / focus distance · torch · portrait + landscape · mirror + flip · brightness/contrast/saturation · background blur / bokeh / mosaic / replace / green screen · face smoothing · PC preview · screenshot · record to PC · configurable bitrate/quality/encoder

**New / improved:**
Arbitrary-angle rotation with fit-crop-pan-zoom and presets · NVIDIA Maxine VFX + AR (AI green screen, blur, super-resolution, upscale, denoise, artifact reduction, relighting, **eye contact**) · ONNX/DirectML segmentation for AMD/Intel · device pairing + optional stream encryption · **background capture on iOS** · OBS source plugin (bypasses the virtual camera round-trip) · latency meter and diagnostics page · thermal-aware auto-downshift · global hotkeys · no watermark, no ads, no tiers

## Architecture

```
iPhone (Swift/SwiftUI)                    Windows 11 (C++20 / Qt 6)
┌───────────────────────┐                 ┌──────────────────────────────────────┐
│ AVCaptureSession      │                 │ rc-core (no UI deps)                 │
│  ↓ manual controls    │   Bonjour       │  transport → decode → pipeline → sink│
│ VTCompressionSession  │   TCP / usbmux  │                                      │
│  HEVC/H.264 low-lat   │ ══════════════▶ │  FFmpeg d3d11va ─┐                   │
│ NWConnection          │                 │                  ▼                   │
│ voip background mode  │ ◀══ control ═══ │  D3D11: Transform → Effects → Compose│
└───────────────────────┘   stats/ABR     │                  │                   │
                                          │      ┌───────────┼──────────┐        │
                                          │      ▼           ▼          ▼        │
                                          │  SHM ring    Recorder   Qt preview   │
                                          └──────┼───────────────────────────────┘
                                                 │ Global\ shared memory
                                    ┌────────────┴────────────┐
                                    ▼                         ▼
                        rc-vcam.dll (Session 0,        obs-remotecam
                        Frame Server, LOCAL SERVICE)   (direct read)
                                    │
                                    ▼
                        Zoom / Teams / Discord / Chrome / OBS
```

### Repo layout (monorepo)

```
remoteCam/
├── docs/{protocol.md, architecture.md, building.md, nvidia.md, sideloading.md}
├── windows/
│   ├── core/          rc-core static lib — transport, decode, pipeline, effects, sink, session
│   ├── vcam/          rc-vcam.dll — MF media source COM server (loaded by Frame Server)
│   ├── register/      rc-vcam-register.exe — elevated one-shot registration helper
│   ├── app/           Qt 6 / QML UI, tray, hotkeys
│   ├── obs-plugin/    obs-remotecam source
│   ├── tools/         rc-fakephone, rc-vcam-probe
│   └── installer/     WiX MSI
├── ios/RemoteCam/     Swift 6, SwiftUI, AVFoundation, VideoToolbox, Network.framework
└── third_party/       nvvfx + nvar MIT headers & proxy loaders, mdns helpers
```

---

## 1. Virtual camera — highest-risk component, build it early

`MFCreateVirtualCamera(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System, MFVirtualCameraAccess_AllUsers, L"RemoteCam", L"{our-clsid}", nullptr, 0, &cam)`

Critical implementation facts established during research:

- It is a **user-mode COM media source — no kernel driver, no WHQL, no driver signing.** This is the single biggest reason to target Win11 only.
- **Register under `HKLM\SOFTWARE\Classes\CLSID\{...}\InprocServer32`, never `HKCR`.** UAC silently redirects `HKCR` writes to a per-user hive that Session 0 cannot read, and the failure mode is a camera that enumerates but never produces frames.
- **The Frame Server (`svchost -k Camera`, LOCAL SERVICE, Session 0) loads `rc-vcam.dll`** — not our app. Frames must cross a session boundary.
- Registration needs admin **once**, at install time via `rc-vcam-register.exe`. Never at app launch.
- MF virtual cameras are visible to **both** Media Foundation and DirectShow consumers, so one implementation covers Zoom, Teams, Discord, Chrome, OBS and the Windows Camera app.

**Frame handoff (`windows/platform/sink/shm_writer.cpp` ↔ `windows/vcam/frame_reader.cpp`):**

- Named section `Global\RemoteCam.Frames.0`, **created by `rc-vcam.dll` in Session 0 and merely opened by the producer** — see [Corrections](#corrections-to-the-original-plan) for why it cannot be the other way round. DACL grants full access to LOCAL SERVICE, read+write to `INTERACTIVE` so the unelevated producer can publish, and read to `ALL APPLICATION PACKAGES` so packaged consumers like the Windows Camera app work.
- Ring of 4 NV12 slots + header (`{magic, version, width, height, fps, format_generation, write_seq, slot_seq[4]}`). Slots are sized for 4K up front (~50 MB total) so a resolution change is a `format_generation` bump rather than a re-created section every reader would have to reopen.
- **Seqlock + auto-reset event** (`Global\RemoteCam.Frame.0`) so the writer never blocks and torn reads are detectable by the reader.
- Frames are **system-memory NV12**, staged out of D3D11 with 3 rotating staging textures and a fence so readback never stalls the render thread. 1080p60 ≈ 187 MB/s, 4K60 ≈ 750 MB/s — acceptable.
- *Optimization spike (not v1):* NT-handle-shared D3D11 textures duplicated into the Frame Server process. Cross-session `DuplicateHandle` into a Session 0 service is likely blocked; measure before committing.

**Format ladder:** advertise a fixed list (3840×2160 / 2560×1440 / 1920×1080 / 1280×720 / 960×540 / 640×480, each at 30 and 60 fps, NV12). Apps pick one; we letterbox/scale into it. **This decoupling is what makes arbitrary rotation practical** — a portrait phone rotated 37° still lands cleanly in the 1080p landscape canvas Zoom asked for.

**Always produce frames.** When no phone is connected, emit a "RemoteCam — waiting for phone" placeholder. Several apps drop or error on a stalled device.

## 2. Rotation — the headline feature

One D3D11 pixel-shader pass, one 3×3 matrix, so generality is free:

```
srcUV = T(srcCenter) · S(1/zoom) · R(−θ) · F(flipX, flipY) · T(−dstCenter) · T(−pan) · dstUV
```

- **Angle**: continuous −180°…+180°, 0.1° precision. ⟲/⟳ buttons snap 90°. Shift-drag snaps to 15°. `[` / `]` nudge ±1°, `Ctrl+[` / `]` step ±90°.
- **Fit mode** — three modes, not four. `Fit` (letterbox, whole source visible) · **`Fill`** (covers the canvas, crops the source, **no empty corners at any angle** — this is the mode that makes odd angles usable) · `Stretch` (non-uniform, fills the frame, ignores aspect).
  - `Fill` is computed by projecting the *canvas* back into source space and requiring that rectangle to fit inside the source: `s = max((dstW·|cosθ| + dstH·|sinθ|)/srcW, (dstW·|sinθ| + dstH·|cosθ|)/srcH)`. **Not** by covering the source's rotated bounding box — see [Corrections](#corrections-to-the-original-plan).
  - Implemented and tested in `core/src/transform.cpp`; the bound is proven tight by test.
- **Flip H / Flip V** independent of angle.
- **Pan + zoom** by dragging and scrolling directly on the preview — how you reframe after rotating. Clamp every drag with `rc::clampPanForCoverage` so `Fill` can never reveal a hard edge. **Do not clamp the two axes independently** — see [Corrections](#corrections-to-the-original-plan).
- **Auto-rotate**: phone sends `UIDeviceOrientation` / `CMMotionManager` attitude on the control channel; drives the 90° component while the manual slider adds an offset on top. Lock toggle to ignore the phone entirely.
- **Presets**: named transforms ("overhead desk cam", "vertical portrait", "sideways tripod").
- **Sampling**: bilinear minimum, Catmull-Rom bicubic option — naive bilinear at non-90° angles looks noticeably soft.

**Ordering matters:** transform runs *before* AI effects. Maxine's segmentation and eye-contact models expect an upright person; rotating afterwards would feed them a sideways face.

## 3. GPU pipeline

```
decoder output (NV12/P010 D3D11 texture)
  → rc::pipeline::Transform    rotate/flip/crop/zoom/scale, NV12→RGBA
  → rc::effects::Chain         ordered, drag-to-reorder in UI
       MaxineVfxEffect         green screen · blur · super-res · upscale · denoise · artifact reduction · relight
       MaxineArEffect          eye contact
       OnnxSegmentationEffect  DirectML fallback → blur / replace / mosaic / color-pop
       ChromaKeyEffect         classic green screen, shader-only, no GPU requirements
       ColorAdjustEffect       brightness · contrast · saturation · gamma · temperature · tint · sharpen
       FaceSmoothEffect        bilateral smoothing gated on the segmentation/face mask
  → rc::pipeline::Compose      letterbox into output canvas, optional overlays
  → RGBA→NV12
  → fan-out: SHM writer · recorder · Qt preview (shared texture, no readback) · OBS
```

```cpp
class IEffect {
public:
  virtual ~IEffect() = default;
  virtual std::string_view id() const = 0;
  virtual bool available() const = 0;                 // SDK + GPU present?
  virtual std::string unavailableReason() const = 0;  // surfaced in the UI
  virtual HRESULT configure(const EffectParams&) = 0;
  virtual HRESULT process(ID3D11Texture2D* in, ID3D11Texture2D* out, FrameMeta&) = 0;
};
```

Backend selection per effect: **auto** (Maxine → DirectML → shader) with manual override. Unavailable effects render greyed with a reason, never hidden.

## 4. NVIDIA integration

Two independent wins, both worth shipping.

**(a) Maxine in-process — the real integration.**

- Vendor the **MIT-licensed** `nvvfx` / `nvar` headers plus `NVVideoEffectsProxy.cpp` and `NVARProxy.cpp` into `third_party/`. These `LoadLibrary` the runtime lazily and fail gracefully when absent — **we ship zero NVIDIA binaries**, so there is no redistribution, licensing, or download-size problem.
- Startup probe: `NVVFX_SDK_DIR` / `NVAR_MODEL_DIR` env vars → default install paths → `cudaGetDeviceProperties` for SM ≥ 7.5 with Tensor Cores. If absent, effects grey out with an "Install the NVIDIA Maxine redistributable" link. Users who already have NVIDIA Broadcast installed have the runtime.
- **Buffer interop** is the part that can eat the latency budget: `cudaGraphicsD3D11RegisterResource` on our textures **once**, cached — never per frame — then map, wrap as `NvCVImage`, `NvCVImage_Transfer` for layout/format conversion, run, transfer back. Measure in M5; be ready to run Maxine on its own thread at a one-frame delay if interop cost is high.
- Respect per-effect constraints: virtual background needs ≥288 px height; SuperRes only does fixed 1.5×/2×/3×/4× scales. The UI must constrain resolution choices, not fail at runtime.
- AFX (audio noise/echo removal) is designed in but not built — it lands with audio post-v1.

**(b) Chaining with the NVIDIA Broadcast app — free, but verify.**

Because we register a proper MF virtual camera, Broadcast can select **RemoteCam** as its input and stack its own effects on top. Test this explicitly and document it, including the Win11 24H2 quirk where two Broadcast cameras appear and users must choose *"Camera (NVIDIA Broadcast)"*, not *"Camera (NVIDIA Broadcast) (Windows Virtual Camera)"*. Also confirm selecting Broadcast's output as our input doesn't create a loop.

## 5. Non-NVIDIA fallback

ONNX Runtime with the **DirectML** execution provider — works on any DX12 GPU including AMD, Intel Arc, Intel iGPUs, and pre-Turing NVIDIA cards. Model: MediaPipe Selfie Segmentation (Apache-2.0, ~250 KB). **Verify the license of any model before vendoring** — Robust Video Matting looks tempting but check its terms first.

Inference runs at 256×144; the mask is upsampled and guided-filter-refined in a shader. That is what makes it cheap enough for 60fps on an iGPU.

## 6. Transport & protocol

`docs/protocol.md` is a first-class deliverable, versioned, with test vectors.

- **Discovery**: PC publishes `_remotecam._tcp` using **`DnsServiceRegister`** (dnsapi.h, built into Windows — no Bonjour redistributable). iOS browses with `NWBrowser`. **Bonjour specifically, not UDP broadcast** — broadcast requires Apple's `com.apple.developer.networking.multicast` entitlement, which needs approval; Bonjour does not. TXT records carry protocol version, PC name, pairing state, capabilities. Manual IP + recent-connections list for networks with client isolation.
- **Framing**: one TCP connection, `TCP_NODELAY`.
  ```
  u32 length │ u8 channel │ u8 flags │ u16 reserved │ u64 pts_us │ payload
  channels: 0=control (CBOR)  1=video  2=audio (reserved, unused in v1)  3=stats
  ```
  Reserving channel 2 now means adding audio later is not a protocol break.
- **USB**: same protocol, different socket. Windows talks to Apple Mobile Device Service's usbmux on `127.0.0.1:27015` (`ListDevices` / `Listen` / `Connect` plist messages) to reach a port the iOS app listens on. **Write a minimal usbmux client (~400 lines)** rather than depending on LGPL libimobiledevice. Detect missing AMDS and tell the user to install Apple Devices / iTunes.
- **Pairing**: PC shows a 6-digit code + QR (QR carries host, port, and code so nothing is typed). Phone confirms → SPAKE2 → both persist a long-term key (iOS Keychain, Windows DPAPI). Reconnects are silent. Per your decision, **media encryption is a default-off toggle**; the control channel is always HMAC-authenticated so settings can't be hijacked, and an unpaired device can never stream.
- **Codecs**: HEVC preferred, H.264 High fallback, negotiated at connect. Decode via FFmpeg **`d3d11va`** — this uses the GPU driver's DXVA decoder, so **HEVC works without the paid Microsoft HEVC Video Extensions**. Using MF's own HEVC decoder would silently require a $0.99 Store purchase.
- **Adaptive bitrate**: receiver reports queue depth, decode latency and loss on channel 3; sender retunes `VTCompressionSession` bitrate.
- **Latency target**: capture 8 + encode 12 + net 5 + decode 8 + process 5 + vcam 5 ≈ **45 ms glass-to-glass at 1080p60**, measured by a built-in meter that round-trips a timestamp through the whole pipeline.

## 7. iOS app

Swift 6 + SwiftUI, minimum iOS 17 (iOS 18 for background capture).

- `AVCaptureSession` with `.builtInWideAngleCamera` / `.builtInUltraWideCamera` / `.builtInTelephotoCamera`, plus the virtual `.builtInTripleCamera` for seamless zoom transitions across lenses.
- Controls: tap-to-focus/expose, long-press AE/AF/AWB lock, manual ISO + shutter + EV + white-balance + focus distance, `videoZoomFactor` with ramping, torch, stabilization mode, low-light boost, HDR.
- Encode: `VTCompressionSession` with `EnableLowLatencyRateControl = true`, `RealTime = true`, `AllowFrameReordering = false`, ~2 s keyframe interval, `AverageBitRate` + `DataRateLimits`.
- **Background capture**: `UIBackgroundModes = ["voip"]`, check `AVCaptureSession.isMultitaskingCameraAccessSupported`, set `isMultitaskingCameraAccessEnabled = true`. Handle `wasInterruptedNotification` with reason `videoDeviceNotAvailableInBackground` explicitly. Live Activity so the user always knows the camera is live.
- **Thermal/battery**: watch `ProcessInfo.thermalStateDidChangeNotification`, auto-downshift resolution/fps at `.serious`. A "dimmed streaming" mode that stops rendering the local preview — the preview costs more battery than the encode does.
- `NSLocalNetworkUsageDescription` + `NSBonjourServices = ["_remotecam._tcp"]`.

## 8. Qt UI (Windows)

Preview composited from the D3D11 texture via `QQuickWindow::beforeRenderPassRecording` / a `QSGRenderNode` — Qt 6's RHI already runs on D3D11 on Windows, so no readback.

Left rail: **Devices** (paired list, pairing flow) · **Video** (resolution, fps, codec, bitrate) · **Transform** (the rotation panel) · **Effects** (reorderable chain, per-effect params, availability badges) · **Output** (canvas size/fps, placeholder image, recording) · **Diagnostics**.

Tray icon, run-at-login, minimize-to-tray, global hotkeys (rotate 90°, freeze, blank, screenshot, record). Follows system dark/light.

**Diagnostics page** — latency meter, dropped frames, bitrate graph, GPU decode/encode load, effect timings, one-click "copy diagnostics". This single page will save more support effort than any other screen.

## 9. OBS plugin

`obs-remotecam` reads the same `Global\` SHM ring directly and registers as an async NV12 video source — skipping the virtual-camera round-trip entirely for lower latency. v1 is a read-only mirror of whatever the app is producing; source-level property control can come later.

## 10. Testing

- **rc-core is UI-free and unit-testable.** Protocol framing round-trips; **transform matrix verified against a CPU reference implementation at many angles with a PSNR floor**; effect-chain ordering; SPAKE2 handshake test vectors.
- **`rc-fakephone`** — CLI that replays a video file over the real protocol. Lets the entire Windows side be developed and CI'd with no iPhone attached. Build this in M0; it pays for itself immediately.
- **`rc-vcam-probe`** — opens the virtual camera through both MF and DirectShow and dumps frames. Proves the Session 0 handoff independently of any third-party app.
- **Manual compatibility matrix** each milestone: Zoom · Teams · Discord · Chrome (Meet) · OBS · Windows Camera. These are the ones that break.

## Milestones

| # | Deliverable | Est. |
|---|---|---|
| **M0** | Walking skeleton: iOS capture → H.264 → TCP → decode → Qt preview, hardcoded IP. Plus `rc-fakephone`. Proves the latency budget on day one. | 2 wk |
| **M1** | **Virtual camera**: MF media source DLL, Session 0 SHM handoff, registration helper, placeholder frames, `rc-vcam-probe`. Verified across the app matrix. *Highest risk — built second, not last.* | 2 wk |
| **M2** | **Transform**: rotation / fit-mode / pan / zoom / flip panel, auto-rotate, presets. Math already done in `core/`; this is the shader plus the UI. | 1 wk |
| **M3** | Bonjour discovery, pairing + QR, usbmux/USB, reconnect, adaptive bitrate. | 2 wk |
| **M4** | Full iOS manual camera controls + lens switching + Windows-side UI. *(iVCam paywalls exactly this.)* | 1.5 wk |
| **M5** | Effect chain: shader effects → ONNX/DirectML segmentation → Maxine VFX → Maxine AR eye contact. | 3 wk |
| **M6** | Record to MP4 (NVENC/MF), screenshot, OBS plugin. | 1.5 wk |
| **M7** | iOS background capture, thermal management, diagnostics page, MSI installer, docs, CI. | 2 wk |

**≈15 weeks.** Post-v1: audio + virtual mic driver, multi-phone → multi-camera, Android client, macOS/Linux host.

## Risks

1. **Session 0 frame handoff (M1)** — the make-or-break unknown. Mitigation: prototype it first; fallbacks are a frame-producing Windows service, or `MFVirtualCameraAccess_CurrentUser` if that keeps the source in-session.
2. **Maxine CUDA↔D3D11 interop latency** — could blow the 45 ms budget. Measure early; fall back to a dedicated thread with one frame of delay.
3. **USB depends on Apple Mobile Device Service.** No AMDS → no USB. Detect and degrade to Wi-Fi with a clear message rather than a mystery failure.
4. **`voip` background mode without actual VoIP** may draw App Review scrutiny. Frame the listing as a live video-link app; be ready to instead request `com.apple.developer.avfoundation.multitasking-camera-access` from Apple.
5. **App Review generally** — iVCam, Camo and DroidCam all ship, so the category is fine, but Local Network + background camera invite questions. Budget review cycles.

## Licensing

Project under **Apache-2.0**. Qt 6 is LGPLv3 → dynamic linking, ship Qt DLLs separately, document relinking. FFmpeg LGPL-2.1 → dynamic link, build **without** `--enable-gpl`. Maxine headers/proxies MIT. ONNX Runtime MIT. **Do not copy from obs-virtual-cam or BestCam — both GPL-2.0.** Read them for understanding; write our own. Verify any ML model's license before vendoring.

## Verification

- `ctest` green in `core/tests` — transform invariants (done), plus framing and pairing vectors when those land.
- `rc-fakephone --file sample.mp4 --connect 127.0.0.1` drives the full Windows pipeline with no phone; preview renders, frame counter advances.
- `rc-vcam-probe --mf` and `--directshow` both pull frames from the registered camera; SHA of a known test pattern matches.
- Open **Zoom, Teams, Discord, Chrome/Meet, OBS, and Windows Camera** — RemoteCam appears in each picker and shows live video.
- Rotate to 37° in `Fill` mode while streaming into Zoom — no black corners, no dropped frames, correct aspect.
- On an RTX machine: enable AI green screen + eye contact, confirm the diagnostics page shows Maxine active and end-to-end latency stays under budget. On an AMD/Intel machine: confirm the same effects run via DirectML and Maxine shows greyed with a reason.
- Lock the iPhone screen mid-call — video keeps flowing (background capture).
- Pull the Wi-Fi, restore it — automatic silent reconnect with no re-pairing.

---

## Status and handoff

### Repository layout as built

The plan originally put the portable core at `windows/core/`. **It lives at `core/`
instead**, top-level and outside `windows/`. The transform math and wire protocol
have no Windows dependency, and burying them under `windows/` would have made the
trickiest logic in the project impossible to test without a Windows machine. The
constraint is now enforced by rule in `core/CMakeLists.txt`: nothing in that target
may reference D3D11, Media Foundation, Qt or WinRT.

```
core/          portable C++20 — transform math (done), protocol, pairing. Builds anywhere.
windows/       Qt 6 client, MF virtual camera, OBS plugin, installer.   Windows agent.
ios/           Swift 6 / SwiftUI capture app.                           Mac agent.
docs/          protocol.md — normative wire spec.
CLAUDE.md      agent operating guide; windows/ and ios/ have their own.
```

### Ownership

| Agent | Owns | Builds | Cannot build |
|---|---|---|---|
| Linux | `core/`, `docs/`, `.github/` | `core/` + tests | anything platform-specific |
| Windows | `windows/` | everything | `ios/` |
| Mac | `ios/` | `ios/` | `windows/` |

`core/` and `docs/protocol.md` are shared surfaces — changes there affect the other
platform agent. Keep them small, commit them separately, and say so in the message.

### Done

**`core/` transform math**, implemented and verified: `rc::transform` produces the
row-major 3×3 backward map (`destToSource`) that the D3D11 pixel shader consumes —
arbitrary-angle rotation, independent flips, three fit modes, zoom, pan, and
coverage-preserving pan clamping. 12,101 assertions pass, including a
264-combination inverse round-trip sweep and a per-degree check that `Fill` never
exposes an empty corner across five source aspect ratios.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j
ctest --test-dir build --output-on-failure
```

Also written: `docs/protocol.md` (draft, unimplemented), `README.md`, `.gitignore`,
and CI that builds `core/` on all three runners.

**M1 — written and building, verification outstanding.** On the Windows box:
`windows/common/` (Win32 helpers, NV12 geometry, the test pattern, the `Global\` frame
ring), `windows/vcam/` (`rc-vcam.dll`, the MF media source), `windows/register/`
(`rc-vcam-register.exe`), and `windows/tools/` (`rc-vcam-probe.exe` for MF **and**
DirectShow, `rc-fakewriter.exe` as a stand-in producer). Everything compiles clean at
`/W4 /permissive-` and `rcwin-common-tests` passes 111 assertions, including a threaded
seqlock contention test that races ~1600 concurrent reads against 3000 writes with zero
torn frames.

What that does **not** yet prove: that Windows loads the DLL, that either stack pulls
frames from it, or that the Session 0 handoff works. Those need an elevated
`--register` and a hand pass over the consumer matrix. Until then M1 is unverified.

### Not done

Everything else.

### Where to pick up

**Windows agent — finish M1's verification.** The code is written; run it. In order:
`rc-vcam-register.exe --register` from an elevated prompt → `rc-vcam-probe --mf` and
`--directshow` → the consumer matrix by hand → `rc-fakewriter.exe` while a consumer is
open, which is the actual Session 0 proof. If that last step fails, **escalate rather
than working around it**; the fallbacks are a small always-on broker service that owns
the section, or `MFVirtualCameraAccess_CurrentUser`, and that choice reshapes the
design.

**Mac agent — M0.** Capture → `VTCompressionSession` → `NWConnection`, streaming to
`rc-fakephone`'s counterpart or a scratch TCP sink, to establish the real capture and
encode latency early. The protocol codec can follow.

**Linux agent — M0/M3 core work.** Protocol framing codec and its round-trip tests,
SPAKE2 pairing, the usbmux plist client, the adaptive-bitrate controller. All
portable, all testable without hardware.

### Corrections to the original plan

Two errors in the first draft, both found by deriving and testing the math rather
than by review. Fixed in §2 above; recorded here so nobody reintroduces them.

**1. There were four fit modes; there are three.** The plan defined `Fill` as
covering the source's *rotated bounding box*, and added a separate `Auto-crop` mode
for the no-black-corners case. Bounding-box fill does not fill: a square source at
45° scaled that way becomes a diamond whose bbox matches the canvas while the canvas
corners stay empty. The correct cover formula is the one written up as `Auto-crop` —
project the canvas back into source space. They are the same operation, so
`Auto-crop` is gone and `Fill` uses that formula. **UI impact: three fit modes.**

**2. Pan cannot be clamped per axis.** Pan is applied in canvas space *after*
rotation, while the available slack is measured along *source* axes. At angles that
are not multiples of 90° a canvas-space drag consumes slack on both source axes at
once, so the permitted region is a rotated rectangle, not an axis-aligned box — there
is no single "maximum panX" to return. `rc::panSlack` reports slack in source space
and `rc::clampPanForCoverage` projects into source axes, clamps, and maps back. A
per-axis clamp passes casual testing and leaks a black wedge at odd angles.

**3. The frame producer cannot create the shared-memory section; the DLL must.** §1
originally had the app's SHM writer create `Global\RemoteCam.Frames.0`. Creating any
object in the `Global\` namespace requires `SeCreateGlobalPrivilege`, which a
non-elevated interactive process does not hold — `CreateFileMapping` returns
`ERROR_ACCESS_DENIED`. Measured on the dev machine:

```
GLOBAL CREATE: FAILED -> Access to the path is denied.
LOCAL CREATE: SUCCEEDED
```

`whoami /priv` shows the privilege absent and `BUILTIN\Administrators` marked *deny
only* under the UAC-filtered token. Elevating a consumer webcam app to work around this
is not an acceptable trade. *Opening* a `Global\` object needs no privilege, only DACL
permission, so the roles are inverted: `rc-vcam.dll` runs inside the Frame Server as
LOCAL SERVICE — which does hold the privilege — and creates the section; the producer
opens it. **Consequence: the ring exists only while some application has the camera
open.** That is not a limitation in practice, because with no consumer there is nobody
to send frames to, but it does mean the producer polls for the ring rather than
expecting it to be there.

Related, measured at the same time: `MFCreateVirtualCamera` returns `E_ACCESSDENIED`
when unelevated **even with `MFVirtualCameraLifetime_Session`**, so session lifetime is
a convenience for dev iteration, not a way to avoid the one-time admin step.

**Convention fixed while implementing:** positive `rotationDeg` is **clockwise as
displayed**, so the UI's "rotate right" button is +90. Rotation is applied about the
source centre, in y-down image coordinates.
