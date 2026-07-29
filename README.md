# RemoteCam

Use your iPhone as a webcam on Windows 11. Free, open source, no watermark, no ads,
no tiers, nothing gated behind a purchase.

**Status: active development.** The iOS capture app and Windows virtual-camera
foundation now build, but a secure end-to-end stream is not available until the
Windows listener/decoder and the pairing contract are completed. See [PLAN.md](PLAN.md)
for milestones and [the iOS/backend handoff](docs/ios-backend-handoff.md) for the
current integration work.

## Why

[iVCam](https://www.e2esoft.com/ivcam/) is the incumbent, and it works, but:

- The free version burns a watermark into your video, shows ads, and drops to
  640×480 after the trial. Manual camera controls — ISO, exposure, white balance,
  focus — are gated behind the top tier specifically.
- **The PC can only flip the image, never rotate it.** Mount your phone sideways or
  overhead, which is the main reason to use a phone as a webcam in the first place,
  and you cannot fix the framing.
- It does nothing with the RTX GPU a lot of its users already own.
- The stream is unauthenticated and unencrypted on your LAN.
- On iOS it cannot stream in the background.

RemoteCam fixes all of that.

## What it does differently

**Rotate to any angle.** Not just 90° steps and not just flips — a continuous
−180°…+180° control with pan, zoom, and a Fill mode that guarantees no black corners
at *any* angle. Optionally follows the phone's own orientation, with the manual
offset composing on top and a lock when you don't want it moving.

**Uses your GPU properly.** On RTX cards, NVIDIA's Maxine SDKs run in-process for AI
green screen, background blur, denoising, super-resolution, and eye contact. We ship
no NVIDIA binaries — the SDK is delay-loaded, so the app runs unchanged without it.
On AMD, Intel, and older NVIDIA cards the same effects run through ONNX Runtime on
DirectML, so the feature list does not depend on your vendor.

**Keeps streaming when you switch apps.** The iOS app declares the background camera
capability, so locking your phone doesn't kill your call.

**Asks before it lets anything connect.** New devices must be approved on the PC via
a 6-digit code. Media encryption is a toggle; control-channel authentication is not
optional.

## Building

The portable core builds and tests anywhere, including Linux:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
cd build && ctest --output-on-failure
```

The Windows client needs Windows 11 (build 22000+), MSVC, and Qt 6. The iOS app
needs Xcode on macOS. Neither cross-compiles; CI builds them on `windows-latest` and
`macos-latest` respectively.

## Layout

| Path | What |
|---|---|
| `core/` | portable C++20: transform math, wire protocol, pairing. No Windows or Qt dependency, by rule. |
| `windows/` | Qt 6 client, Media Foundation virtual camera, OBS plugin, installer |
| `ios/` | Swift 6 / SwiftUI capture app |
| `docs/` | [wire protocol](docs/protocol.md) and architecture notes |

The split is load-bearing: keeping the transform math and protocol free of platform
APIs is what lets the trickiest logic be unit-tested on any machine, including CI
runners with no GPU.

RemoteCam has no cloud service, analytics, ads, or tracking. See the
[privacy document](docs/privacy.md) for the data exchanged directly between the
phone and the selected PC.

## Licence

Apache-2.0. See [PLAN.md](PLAN.md) for third-party licence constraints — in
particular, no code is taken from the GPL-licensed virtual camera projects.


Committed to local `main` at `28198be` (`windows: harden vcam lifetime and diagnostics`). Build succeeded and both test suites passed. `main` is three commits ahead of `origin/main`; nothing was pushed. The untracked root `AGENTS.md` was left untouched.

Prompt for the front-end agent:

```
Update the Windows Qt/QML front end to comply with the frame-ring contracts now on main at commit 28198be.

Read windows/API-NOTES.md and use windows/tools/fakewriter/main.cpp as the reference producer implementation.

Required changes:

1. The app is a producer. It must call FrameRing::open(true), never create(). Poll while no consumer has the virtual camera open. Treat ERROR_FILE_NOT_FOUND as a normal “Waiting for a camera app…” state, not an error.

2. The ring can disappear whenever the camera consumer closes. Detect write/open failures, close the stale handle, return to the waiting state, and retry. Reset the frame-pacing origin after reconnect so the app does not publish a catch-up burst.

3. Enforce one running Qt producer per user session with a Local\ named mutex, analogous to rc-fakewriter. Do not use Global\, since the unelevated app lacks SeCreateGlobalPrivilege. If another instance exists, show/activate it or present a friendly explanation.

4. For M1, publish exactly NV12 1920×1080 at 30 fps. Disable or clearly mark unsupported output format controls for now. The virtual camera currently rejects mismatched geometry; do not add scaling inside FrameSource.

5. Publish valid metadata: even dimensions, stride >= width, format kFourccNv12, and bytesUsed sufficient for stride*height*3/2 and within kRingSlotBytes.

6. Pace publishing with a high-resolution waitable timer against a fixed origin, not Sleep(1000/fps). Reset that origin on every reconnect.

7. Make connection states explicit in the UI:
   - Waiting for camera consumer
   - Connected / publishing
   - Producer conflict
   - Actual failure
   Avoid alarming notifications for the normal waiting/disconnect cycle.

8. Do not log per-frame failures continuously. Log state transitions once and recovery once, consistent with the capped/rotated logging policy in API-NOTES.md.

Keep changes under windows/app and the minimum necessary Windows integration code. Do not modify core/ or docs/protocol.md. Build with:
  cmake --build build --config Debug --parallel
  ctest --test-dir build -C Debug --output-on-failure

Do not claim M1 is working until the elevated registration and live consumer matrix have been manually verified.
```
