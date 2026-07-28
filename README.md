# RemoteCam

Use your iPhone as a webcam on Windows 11. Free, open source, no watermark, no ads,
no tiers, nothing gated behind a purchase.

**Status: early development.** Nothing is usable yet. See [PLAN.md](PLAN.md) for the
full design and milestones.

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

## Licence

Apache-2.0. See [PLAN.md](PLAN.md) for third-party licence constraints — in
particular, no code is taken from the GPL-licensed virtual camera projects.
