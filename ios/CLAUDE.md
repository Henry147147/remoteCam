# Mac agent

Owns `ios/`. Read the root [CLAUDE.md](../CLAUDE.md) first — particularly the Bonjour
and background-camera facts, which constrain the design more than anything in this
file.

## Toolchain

Xcode (current), Swift 6, SwiftUI. Minimum deployment iOS 17; background capture
needs iOS 18. A **physical device is required** — the simulator has no camera, so
capture, encoding and thermal behaviour can only be verified on hardware.

Distribution: built for App Store-grade entitlements, but the project must stay
self-signable so anyone can sideload from source. Do not add anything that hard-fails
without a paid team ID.

## Frameworks

`AVFoundation` capture · `VideoToolbox` hardware encode · `Network.framework`
(`NWBrowser`, `NWConnection`, `NWListener`) · `CoreMotion` for orientation.

## Non-negotiables

**Bonjour only for discovery.** `NWBrowser` on `_remotecam._tcp`. Never send to a
broadcast or multicast address — that needs
`com.apple.developer.networking.multicast`, which is granted by Apple on review.
Requires `NSLocalNetworkUsageDescription` and
`NSBonjourServices = ["_remotecam._tcp"]` in Info.plist.

**Background capture** comes from `UIBackgroundModes = ["voip"]`, which makes
`AVCaptureSession.isMultitaskingCameraAccessSupported` true on iOS 18+. Set
`isMultitaskingCameraAccessEnabled = true`, and handle
`AVCaptureSession.wasInterruptedNotification` with reason
`videoDeviceNotAvailableInBackground` explicitly rather than letting the session die
silently. Ship a Live Activity so the user always knows the camera is live — a
webcam app that captures invisibly is a trust problem, not a feature.

**Encoder settings** that actually matter for latency:
`kVTVideoEncoderSpecification_EnableLowLatencyRateControl = true`,
`kVTCompressionPropertyKey_RealTime = true`, `AllowFrameReordering = false`,
keyframe interval ~2 s, `AverageBitRate` plus `DataRateLimits`. Prepend VPS/SPS/PPS
to **every** keyframe — the PC must be able to join or recover mid-stream with no
side channel.

**Orientation is reported, never obeyed.** Send `UIDeviceOrientation` /
`CMMotionManager` attitude on the control channel and let the PC decide. The user's
manual rotation offset composes on top and a PC-side lock ignores the field. A phone
that redecides the output framing every time someone nudges the tripod is unusable.

## Battery and thermals

Subscribe to `ProcessInfo.thermalStateDidChangeNotification` and auto-downshift
resolution and fps at `.serious`. Honour the PC's `set_preview {enabled: false}`
message by stopping local preview rendering — that is the single largest power saving
available, larger than the encode itself.

## Protocol

[docs/protocol.md](../docs/protocol.md) is **normative**. If the implementation and
that document disagree, the document is right. Changing it affects the Windows agent,
so raise it rather than editing unilaterally.

Framing is a 16-byte header, all integers big-endian; channel 2 is reserved for audio
and must be **ignored, not rejected**, by a v1 receiver.

## Current status — 2026-07-28

The Swift 6 project is in `RemoteCam.xcodeproj`. Implemented: Bonjour/manual/recent
connections, reconnect, bounded framing, deterministic CBOR, multi-lens AVCapture,
tap focus and manual controls, low-latency H.264/HEVC Annex-B encode, stats bitrate,
orientation/battery/thermal reporting, PC-driven preview saving, diagnostics,
multitasking capture setup, and Live Activity.

The simulator suite reports 16 passing tests and one expected VideoToolbox skip. On
2026-07-28, a signed Debug build made with the beta Xcode/iPhoneOS 27 toolchain was
installed and launched on a physical iPhone. This verifies signing, installation,
and startup only. Do not describe capture, encode, background operation, thermals,
or the Windows end-to-end path as verified until their manual matrices pass.

Production pairing, authenticated control/media encryption, and USB remain blocked
by underspecified shared protocol decisions. Release rejects unauthenticated
`ready`; a Debug-only `--allow-insecure-session` launch argument exists solely for
the fake-PC development harness. See `../docs/ios-backend-handoff.md`.

## Verification

Device-only, by hand:

- Streams to the Windows client over Wi-Fi and over USB.
- Lock the screen mid-call — video keeps flowing.
- Drop Wi-Fi and restore it — silent reconnect, no re-pairing.
- Switch lenses and run the manual controls (ISO, exposure, WB, focus, zoom, torch)
  while streaming; the PC's echoed `camera_state` tracks them.
- Sustained 1080p60 for 20+ minutes: watch thermal state and confirm the downshift
  fires rather than the session dying.

Report what you actually ran on which device. "Compiles" is not "works".
