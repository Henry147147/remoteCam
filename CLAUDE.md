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

**"Agent" above means a machine with a human at it, not a subagent.** Do not spawn a
"Windows agent" or "Mac agent" — you cannot acquire a toolchain you do not have. When
work belongs to another host, write it down (commit message, issue, or the status
section here) and stop.

## How to work in this repo

Tuned for Claude Opus 5 per Anthropic's
[Opus 5 prompting guide](https://platform.claude.com/docs/en/build-with-claude/prompt-engineering/prompting-claude-opus-5).
Effort `high` is the right default here; `xhigh` for a multi-file feature or a
cross-tree refactor, `low`/`medium` for a scoped edit or a question about the code.

**Scope.** Deliver what was asked, at the scope intended. Make routine judgment calls
yourself; check in only when different readings lead to materially different work. If
the request looks mistaken or a better approach exists, say so in a sentence and
continue with the task as asked rather than quietly narrowing, widening, or
transforming it. Finish the whole task, and stop short of what was clearly not asked.
A bug fix does not need the surrounding code cleaned up; a new component does not need
configurability nobody requested; only validate at real boundaries (wire input, OS
calls), not between our own functions.

**Delegation.** Delegate only for large, genuinely independent, parallelizable
tracks — a wide multi-file investigation across `windows/` *and* `ios/`, say. Do not
delegate what you can finish in a handful of tool calls, do not use a subagent to
check your own work, and prefer one subagent over several. A `grep` beats spawning an
explorer.

**Self-verification.** You already check your own work; no instruction here asks you
to double-check, re-verify, or add a final verification pass. The `## Verification`
sections in `windows/CLAUDE.md` and `ios/CLAUDE.md` are a different thing: they are
hardware and consumer matrices a **human** must run on real devices, listed because
unit tests cannot reach them. Never mark those done from a clean build.

**Grounding.** Never make a claim about code you have not opened — this handbook
records decisions, not current line numbers, and the status section below can lag the
tree. Read the file before answering questions about it.

**Progress updates.** One sentence before your first tool call saying what you are
about to do. While working, speak up only on a real finding or a change of direction.
When you finish, lead with the outcome — first sentence answers "what happened" — and
put supporting detail after it. Correct an earlier statement only when the error would
change the reader's code or decisions; otherwise fix it and move on.

**Written output.** This repo already carries ~1,000 lines of Markdown handbook and
plan. Record state by editing the status sections and by committing, not by adding new
report, summary, or handoff files — a new `.md` is a decision to maintain it forever.
Match a document's length to its substance: no filler sections, no restating the plan
back, no boilerplate. Same for code comments — comment the *why* where a simpler
approach is wrong, and nowhere else. Delete scratch files and throwaway scripts when
you are done with them.

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
- **`rc-vcam.dll` links the static CRT** and must keep doing so. Session 0 gives no
  guarantee that `msvcp140.dll` is installed, and app-local copies beside the DLL are
  not dependably on the loader's search path for a DLL the Frame Server loads. A
  missing CRT there is a camera that registers, enumerates, and never delivers a
  frame — the same symptom as the `HKCR` bug and just as expensive to find. Qt is
  built against the dynamic CRT and MSVC will not link the two (LNK2038), which is
  why `windows/common/` builds `rcwin-common` twice.

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
CI builds `core/` on Ubuntu and macOS, the Windows targets/tests on Windows, and the
iOS app plus protocol/storage tests on macOS. Camera capture and hardware encoding
still require a physical-device pass.

## Current status

**Done and verified in automated tests** — the portable transform and protocol
layers. `rc::transform` produces the exact row-major 3×3 backward map consumed by
the D3D11 shader. The bounded deterministic-CBOR codec, 16-byte stream framing,
control messages, and H.264/HEVC Annex-B validation also live in `core/`. The five
core executables pass 32,438 checks on Windows; the same suite passes under
ASan+UBSan in WSL. `clang-tidy`, `/W4 /permissive- /WX`, and the MSVC `/analyze`
configuration are clean.

**Written and building, not yet verified against a live camera consumer** — M1.
`rc-vcam.dll`, the registration helper, MF/DirectShow probe, stand-in producer, and
the `Global\` frame ring all compile. `rcwin-common-tests` now passes all 186 checks,
including consumer replacement, producer crash recovery, geometry renegotiation,
and threaded seqlock contention. The remaining proof needs an elevated
`--register`, the consumer matrix, and the real Session 0 handoff. Until that is run,
**do not describe M1 as working**.

**Windows receive seams implemented and tested, but not a live production path.**
The dual-stack app listener binds TCP 7890 before advertising Bonjour, accepts one
phone with `TCP_NODELAY`, and uses the shared wire/control codecs. `rcwin-backend`
owns the auth-gated session states, hello/progress/idle timeouts, bounded 8-AU/20-MiB encoded
queue, keyframe recovery, metrics, and observer/consumer seams shared by the app and
tests. The production app responds to `hello` with `paired: false` and intentionally
withholds `ready`; it never opens an unauthenticated streaming session. Debug
`rc-fakepc --allow-insecure-session` remains the iOS walking skeleton.
`rc-fakephone` is the external TCP iPhone emulator: phone-side CBOR, camera profiles,
telemetry/control echoes, synthetic/replayed Annex-B, PCG32 chaos, scenarios,
NDJSON/JUnit, and real loopback integration are automated. `RemoteCam-E2E.exe` uses
the same Qt UI/backend with a trust bypass compiled into that non-installed test target
only. The native UI Automation harness verifies streaming and production-lock
checkpoints and intentionally reports the absent live preview/full controls as gaps.
The platform library
contains a D3D11VA FFmpeg decoder factory,
NV12 D3D11 transform, PTS-preserving pipeline, and ABR controller. It compiles
against pinned LGPL FFmpeg 8.1.2 and its seam tests pass, but no decoder/GPU hardware
path or iPhone-to-virtual-camera stream has been run.

**iOS client written and building, not device-verified.** `ios/RemoteCam.xcodeproj`
contains Bonjour/manual/recent connections, framed TCP and deterministic CBOR,
AVCapture multi-lens/manual controls, low-latency H.264/HEVC VideoToolbox encode,
reconnect, telemetry, background multitasking setup, preview power saving, and a
Live Activity. The simulator suite has 12 passing tests and one hardware-encoder
skip; Release compiles for the iPhoneOS SDK. A clean signed install is blocked by
missing local provisioning and Developer Mode. Secure pairing, authenticated
control/media encryption, and USB are blocked on the joint decisions listed in
`docs/ios-backend-handoff.md`; Release rejects unauthenticated streaming.

**Installer packaged locally, elevated install still unverified.**
`RemoteCam-0.1.0-win64.exe` was generated with CPack + NSIS, its archive integrity
was checked, and the staged self-contained Qt app passed a startup smoke test. The
installer registers/unregisters the virtual camera and adds/removes a private-profile
inbound firewall rule for TCP 7890. It has not been elevated and run through the
Windows consumer matrix, so packaging success is not system integration proof.

**Not done** — normative pairing/authentication/media encryption, connecting the
production decoder/transform output to the frame ring, USB, effects, OBS, recorder,
live Qt preview/full controls, and physical iPhone/Windows/GPU verification. A native
desktop audit has run, but correct displayed phone video cannot pass until the preview
exists; do not infer video correctness from the E2E wire-state pass.

Next: settle and implement the shared security contract, join the tested receive
seams into an authenticated end-to-end path, then run the M1 and physical-device
matrices in `windows/CLAUDE.md` and `ios/CLAUDE.md`.

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

<reminder>
Keep replies and written documents reasonably concise. Stay inside the scope asked
for. Delegate rarely. Claim only what you ran.
</reminder>
