# Fake iPhone and Windows desktop verification

`rc-fakephone.exe` is a non-shipping C++20 client that behaves like RemoteCam's iOS
app over a real TCP connection. It is meant to find backend, protocol, recovery, and
desktop-integration defects without requiring an iPhone for every run. It is not a
GUI phone simulator and does not emulate iOS APIs.

Testing is part of the product here, not a demonstration. A green build, a connected
socket, or an open window is not proof that video is correct. The automated layers
are deliberately separate so reports cannot blur wire conformance, backend behavior,
native UI state, decoded pixels, virtual-camera output, and physical-device behavior.

## Security boundary

The simulator follows the production trust rule:

- It refuses an unsigned `ready` unless `--allow-insecure-session` is present.
- `production-lock` expects `server_info {paired:false}`, no `ready`, and zero video.
- The shipping `RemoteCam.exe` links only `RejectingTrustPolicy` while pairing remains
  underspecified.
- `RemoteCam-E2E.exe` contains a test trust policy compiled into that target only. It
  is built only with `RC_BUILD_TESTS=ON` and is never installed or packaged.
- Neither test path invents SPAKE2, HMAC, nonce, or key-storage details that are still
  open in [the backend handoff](../ios-backend-handoff.md).

## Build and fast start

```powershell
cmake -S . -B build -A x64 -DRC_BUILD_TESTS=ON -DRC_BUILD_QT_APP=OFF
cmake --build build --config Debug --parallel
ctest --test-dir build -C Debug --output-on-failure

build\bin\rc-fakephone.exe run `
  --connect 127.0.0.1:7890 `
  --scenario production-lock `
  --report-jsonl test-artifacts\production-lock.jsonl `
  --report-junit test-artifacts\production-lock.xml
```

For an explicitly insecure Debug walking skeleton, start `rc-fakepc` and then run
the conformance suite:

```powershell
build\bin\rc-fakepc.exe --allow-insecure-session --no-bonjour --port 7891
build\bin\rc-fakephone.exe suite `
  --connect 127.0.0.1:7891 `
  --suite conformance `
  --allow-insecure-session `
  --seed 42 `
  --report-jsonl test-artifacts\conformance.jsonl `
  --report-junit test-artifacts\conformance.xml
```

## Commands and options

| Command | Purpose |
|---|---|
| `suite --suite smoke\|conformance\|chaos\|soak` | Run a predefined collection. |
| `run --scenario NAME` | Run one deterministic scenario. |
| `run --script FILE.rcscenario` | Run a small, reviewable scenario script. |
| `replay --file FILE --codec h264\|hevc` | Replay an AUD-delimited Annex-B elementary stream. |
| `discover` | Browse `_remotecam._tcp.local` with the Windows DNS-SD API. |
| `shell` | Repeatedly run named scenarios against one endpoint. |

Common options are `--connect HOST[:PORT]` (bracket IPv6 when supplying a port),
`--seed`, `--duration` in seconds, `--profile standard|constrained`, `--device-id`,
`--no-realtime`, `--report-jsonl`, and `--report-junit`.

Exit codes are stable for scripts: `0` pass, `1` assertion/protocol failure, `2`
usage/configuration, `3` connection failure, and `4` secure-pairing barrier.

NDJSON uses the `rc-fakephone.event.v1` schema. Each line has elapsed milliseconds,
level, event kind, and detail. JUnit contains one testcase per checkpoint rather than
one opaque testcase for an entire multi-second run.

## Stateful behavior

The standard profile has a stable 16-lowercase-hex identity, front and rear camera
descriptors, ultra-wide/wide/tele/TrueDepth lenses, H.264 and HEVC, 720p30 through
1080p60 formats, camera controls, orientation, battery, and thermal telemetry. The
constrained profile exposes H.264-only 720p30, serious thermal pressure, and low
battery.

The state machine sends the same CBOR shapes as
`ios/RemoteCam/Sources/Wire/ControlMessage.swift`:

```text
connect -> hello -> server_info/trust -> ready
        -> caps + camera_state + stream_start + telemetry
        -> Annex-B access units + control echoes + stats reactions
```

It sets `TCP_NODELAY`, resolves IPv4 and IPv6, uses the shared 16-byte decoder, keeps
the 20 MiB video budget, rebases monotonic PTS, repeats parameter sets on synthetic
keyframes, echoes camera changes, reacts to format/preview/keyframe/stats commands,
and reproduces chaos decisions with pinned PCG32 output. Reconnect uses the same
device identity and the first iOS backoff interval.

Scenarios currently cover smoke, the production trust lock, all control families,
adaptive bitrate, reconnect, fatal framing errors, additive/isolated control errors,
keyframe recovery, backpressure, deterministic chaos, and soak.

`.rcscenario` supports `scenario`, `duration_ms`, `seed`, `profile`, `orientation`,
`thermal`, and `run`. Examples live under `windows/tools/fakephone/scenarios/`.

## Media fixtures

Synthetic access units are intentionally structural: they exercise Annex-B, NAL
types, PTS, keyframe flags, parameter-set recovery, framing, queues, and reports. They
are not claimed to be visually decodable.

Generate real video fixtures for decoder/pixel tests with:

```powershell
windows\tools\fakephone\scripts\generate-video-fixtures.ps1 `
  -OutputDirectory test-artifacts\video-fixtures `
  -RequireAllCodecs
```

The script requests Windows Media Foundation's `h264_mf` and `hevc_mf` encoders. It
does not fall back to GPL software encoders. Each output has a fixed upper color
reference, moving lower marker, exact frame number, AUD boundaries, a two-second GOP,
and repeated parameter data. It uses `ffprobe`, fully decodes each result, hashes it,
and writes `manifest.json`. A machine without a HEVC encoder records that fixture as
unavailable; `-RequireAllCodecs` makes that a failing lab prerequisite.

Raw replay expects AUD-delimited Annex-B. A capture with no AUD is treated as one
access unit, which remains useful for keyframe recovery but is not a full stream.

## Native desktop verification

Build Qt with `RC_BUILD_TESTS=ON`, then run:

```powershell
windows\tests\desktop-e2e.ps1 `
  -ReplayFile test-artifacts\video-fixtures\remotecam-1280x720-30fps-h264.h264 `
  -ReplayCodec h264
```

This is native Windows UI Automation plus `PrintWindow`, not browser automation. It
launches the real Qt E2E executable, uses stable QML automation IDs, waits for the
`Phone streaming` checkpoint, captures the target window even when occluded, retains
phone NDJSON/JUnit, then launches the shipping executable on loopback and verifies the
pairing boundary. The loopback test mode neither advertises Bonjour nor changes the
firewall.

The script also inventories every required feature surface: live preview; lens,
zoom, focus, exposure, white balance, torch, and stabilization; rotation, scaling,
pan, flips, lock, and presets; effects; freeze/blank/placeholder; screenshot and
recording; diagnostics; tray; and hotkeys. Missing surfaces are `missing`, never
passes. Correct displayed video additionally requires two retained frames from a
real fixture so color, orientation, aspect, movement, and frame-marker progression
can be checked.

By default any missing capability makes the script exit nonzero. `-AllowKnownGaps`
is an audit mode for capturing evidence on an incomplete build; it does not convert
gaps into passes. Evidence is written under `test-artifacts/desktop-e2e/` using the
`remotecam.desktop-evidence.v1` manifest.

Native computer-use verification is required before a desktop-video claim. If the
execution environment has no native desktop-control capability, record the desktop
result as unverified. Browser automation is supplemental only.

## What still requires a lab pass

- Correct decoded pixels in a real live-preview surface; that UI/pipeline is not yet
  implemented in the current app.
- HEVC on a machine with a legal available decoder and fixture encoder.
- Display scaling at 100%, 125%, 150%, and 200%, multiple GPUs, sleep/wake, and remote
  desktop behavior.
- The elevated virtual-camera Session 0 path and Zoom, Teams, Discord, Chrome/Meet,
  OBS, and Windows Camera consumers.
- A physical iPhone for camera, background, reconnect, LAN discovery, and thermal
  behavior.

Do not replace any of those with “the build passed” or “the window opened.”
