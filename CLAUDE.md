# RemoteCam — agent handbook

Open-source iPhone-as-webcam for Windows 11. Free, no watermark, no ads, no tiers.
Replaces [iVCam](https://www.e2esoft.com/ivcam/).

Read [PLAN.md](PLAN.md) for the full design and milestones, and
[docs/protocol.md](docs/protocol.md) for the wire format. This file is the operating
guide: what is decided, what is already known, and what not to re-derive.

## Three agents, three machines

The project cannot be built on any single host. Work is split by platform:

| Agent | Owns | Can build | Cannot build |
|---|---|---|---|
| **Linux** | `core/`, `docs/`, `.github/` | `core/` + tests | anything platform-specific |
| **Windows** | `windows/` | everything (Win 11 + MSVC + Qt 6) | `ios/` |
| **Mac** | `ios/` | `ios/` (Xcode) | `windows/` |

`core/` is consumed by both platform agents. **Changing `core/` or
`docs/protocol.md` affects the other agent's work** — say so explicitly in the commit
message and keep those changes small and separately committed. Platform agents should
prefer opening an issue over unilaterally reshaping `core/`.

Per-platform detail lives in `windows/CLAUDE.md` and `ios/CLAUDE.md`, which load
automatically when working in those trees.

## Locked decisions — do not relitigate

These were settled deliberately by the project owner after research. Each removes a
large amount of work or cost. If one seems wrong, raise it; do not quietly work
around it.

- **Windows 11 only** (build 22000+). `MFCreateVirtualCamera` only — no DirectShow
  filter, no kernel driver, no driver signing. The frame sink sits behind an
  interface so a DirectShow backend could be added later.
- **No audio in v1.** Avoids the signed virtual-mic driver entirely. The protocol
  reserves channel 2 so adding audio later is not a breaking change.
- **C++20 core + Qt 6 / QML** for the Windows UI.
- **ONNX Runtime + DirectML** fallback so AMD/Intel users get the same effects as
  RTX users. Effects are never vendor-exclusive in the UI.
- **Pairing required; stream encryption optional (default off).** The control
  channel is *always* HMAC-authenticated regardless of that toggle.
- **Multi-phone → multi-camera is post-v1.** Keep the architecture from foreclosing
  it; do not build it.

## Hard-won facts — do not re-derive

Researched at project start. Re-discovering any of these costs hours.

**Windows virtual camera**
- `MFCreateVirtualCamera` is a **user-mode COM media source**. No WHQL, no driver
  signing, no kernel code. Windows 11 build 22000+, `mfsensorgroup.dll`.
- **Register under `HKLM\SOFTWARE\Classes\CLSID\{...}\InprocServer32`, never
  `HKCR`.** UAC silently redirects `HKCR` to a per-user hive that Session 0 cannot
  read. Failure mode is a camera that enumerates but never delivers a frame.
- **The Frame Server loads our DLL, not our app** — `svchost.exe -k Camera`, running
  as LOCAL SERVICE in **Session 0**. Frames must cross a session boundary via
  `Global\`-prefixed shared memory and events with a DACL granting LOCAL SERVICE.
- **The DLL creates the `Global\` section; the app opens it — not the other way
  round.** Creating anything in the `Global\` namespace needs
  `SeCreateGlobalPrivilege`, which a non-elevated interactive process does **not**
  hold: `CreateFileMapping` fails with `ERROR_ACCESS_DENIED`. Verified on the dev box —
  `whoami /priv` lists no such privilege and `BUILTIN\Administrators` is *deny only*
  under the UAC-filtered token. *Opening* a `Global\` object needs no privilege, only
  DACL permission, so `rc-vcam.dll` (LOCAL SERVICE, which does hold it) calls
  `create()` and the producer calls `open()`. Consequence: the ring only exists while
  some consumer has the camera open. Elevating the app is not an option.
- **`MFCreateVirtualCamera` needs elevation even for `MFVirtualCameraLifetime_Session`.**
  Measured: it returns `E_ACCESSDENIED` unelevated. Session lifetime is still the right
  dev loop — it leaves nothing behind — but it is not an escape from the admin
  requirement.
- **Include the KS headers *after* the COM/MF ones.** `ks.h` macro-defines `GUID_NULL`
  to `__uuidof(struct GUID_NULL)`; if `cguid.h` is parsed afterwards its own
  `extern const GUID GUID_NULL` expands into that macro and fails to compile *inside a
  Windows SDK header*, so the error reads as an SDK bug rather than an include-order
  one.
- **`qedit.h` is gone from the modern SDK**, but `CLSID_SampleGrabber`
  (`{C1F400A0-…}`) and `CLSID_NullRenderer` (`{C1F400A4-…}`) are still registered on
  Windows 11. Redeclare `ISampleGrabber` / `ISampleGrabberCB` locally — the vtables and
  IIDs are frozen by COM and cannot change.
- MF virtual cameras are visible to **both** Media Foundation and DirectShow
  consumers, so one implementation covers Zoom, Teams, Discord, Chrome, OBS and the
  Windows Camera app.
- Registration needs admin **once at install time**, never at app launch.

**Codecs**
- Media Foundation's HEVC decoder requires the user to buy *HEVC Video Extensions*
  from the Microsoft Store. **Use FFmpeg's `d3d11va` hwaccel instead** — it goes
  through the GPU driver's DXVA decoder and has no paid dependency. Putting a
  purchase in the path of a project whose point is that nothing is paid would be
  self-defeating.

**NVIDIA**
- Maxine headers and `NVVideoEffectsProxy.cpp` / `NVARProxy.cpp` are **MIT** and
  delay-load the runtime. **Ship no NVIDIA binaries.** The app must run unchanged
  when the SDK is absent — effects grey out with a reason, never disappear.
- Requires RTX Turing or newer with Tensor Cores (SM ≥ 7.5).
- **Register D3D11 resources with CUDA once and cache them.** Per-frame
  `cudaGraphicsD3D11RegisterResource` will destroy the latency budget.

**iOS**
- **Discovery must use Bonjour, never UDP broadcast or multicast.** Broadcast
  requires Apple's `com.apple.developer.networking.multicast` entitlement, granted
  by review. Bonjour browsing is exempt.
- Background camera comes from declaring the `voip` `UIBackgroundMode`, which makes
  `AVCaptureSession.isMultitaskingCameraAccessSupported` true on iOS 18+.
- USB goes through Apple Mobile Device Service's usbmux on `127.0.0.1:27015`. No
  AMDS installed means no USB path at all — detect and say so.

**Licensing**
- Project is **Apache-2.0**.
- **Do not copy code from obs-virtual-cam or BestCam — both GPL-2.0.** They are
  useful to read for understanding; write our own.
- Qt 6 LGPLv3 → dynamic linking only. FFmpeg LGPL-2.1 → build **without**
  `--enable-gpl`. Verify any ML model's licence before vendoring.

## Conventions

- **`core/` must not reference D3D11, Media Foundation, Qt, WinRT, or any platform
  API.** This is the rule that keeps the transform math and protocol testable on a
  machine with no GPU. If something in `core/` needs a platform capability, it is in
  the wrong layer — inject an interface instead.
- Tests are dependency-free (no GTest/Catch). A plain `main()` returning non-zero on
  failure, wired to CTest. Keeps the core buildable anywhere with zero setup.
- **Test geometric and behavioural invariants across a swept range**, not
  spot-checked values. `core/tests/transform_test.cpp` is the reference for the
  style; the pan-clamping bug it caught was invisible to spot checks.
- Comments explain *why*, especially where a simpler-looking approach is wrong. See
  the `FitMode::Fill` comment in `core/src/transform.cpp`.
- British/American spelling: match surrounding code, don't churn it.

## Build and test

Portable core, on any host including Linux:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

`windows/` is added only when `WIN32`, so this configure step works everywhere.
CI builds `core/` on `ubuntu-latest`, `windows-latest` and `macos-latest`; the
platform jobs in `.github/workflows/ci.yml` are stubbed behind `if: false` until
their directories have content.

## Current status

**Done and verified** — `core/` transform math. `rc::transform` produces the 3×3
backward map (`destToSource`) that the D3D11 pixel shader consumes: arbitrary-angle
rotation, independent flips, three fit modes, zoom, pan, and coverage-preserving pan
clamping. 12,101 assertions pass, including a 264-combination inverse round-trip
sweep and a per-degree check that Fill never exposes an empty corner. Also builds and
passes under MSVC, not only GCC.

**Written and building, not yet verified against a live camera** — M1. `rc-vcam.dll`
(MF media source), `rc-vcam-register.exe`, `rc-vcam-probe.exe` (MF *and* DirectShow),
`rc-fakewriter.exe`, and the `Global\` frame ring in `windows/common/`. 111 assertions
pass in `rcwin-common-tests`, including a threaded seqlock contention test. The
end-to-end check needs an elevated `--register` and a hand pass over the consumer
matrix; until that has been run, **do not describe M1 as working**.

**Not started** — everything else.

Next: finish M1's verification (see `windows/CLAUDE.md`), then **M0**'s walking
skeleton and `rc-fakephone`.

## Two corrections already applied — PLAN.md's original text was wrong

1. **There are three fit modes, not four.** The plan listed `Fit`, `Fill`,
   `Stretch` and `Auto-crop`, defining `Fill` as covering the source's rotated
   bounding box. That does not cover: a square source at 45° scaled that way becomes
   a diamond, leaving the canvas corners black. The formula that actually covers is
   the one originally written up as `Auto-crop` — project the *canvas* back into
   source space and require it to fit inside the source. They are the same
   operation, so `Auto-crop` is gone and `Fill` uses that formula.
2. **Pan cannot be clamped per axis.** Pan is applied in canvas space *after*
   rotation, while slack is measured along *source* axes, so at angles that are not
   multiples of 90° a canvas-space drag consumes slack on both source axes at once.
   The permitted region is a rotated rectangle, not an axis-aligned box.
   `clampPanForCoverage` projects into source axes, clamps there, and maps back.

Also fixed by convention: **positive `rotationDeg` is clockwise as displayed**, so
the UI's "rotate right" button is +90.
