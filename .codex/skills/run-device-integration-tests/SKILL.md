---
name: run-device-integration-tests
description: Prepare, monitor, capture, sanitize, diagnose, and publish RemoteCam real-device integration tests across a physical iPhone and Windows 11 host. Use when testing iOS-to-Windows discovery, manual connection, TCP/protocol handshake, pairing gates, camera authorization/capture, VideoToolbox encoding, reconnect/background behavior, Windows decode/frame-ring/virtual-camera delivery, or consumer compatibility; when collecting iPhone console and `%ProgramData%\RemoteCam\logs` evidence; or when investigating reports such as no discovered PC, no camera prompt, connection resets, black video, missing frames, or a camera consumer failure.
---

# Run RemoteCam Device Integration Tests

Use one time-correlated capture to identify the last boundary proven on both devices. Preserve raw evidence outside Git; commit only a concise sanitized sample.

## Establish scope and ground truth

1. Read `CLAUDE.md`, `PLAN.md`, `docs/protocol.md`, `ios/CLAUDE.md`, and `windows/CLAUDE.md`. Read `windows/API-NOTES.md` before changing `windows/common/`.
2. Confirm whether the user authorized monitoring, diagnosis, logging changes, behavioral fixes, publishing, or some combination. Monitoring or diagnosis alone does not authorize a fix or GitHub write.
3. Run `git status -sb`. Preserve unrelated and user-owned changes, especially local Xcode signing/team edits in `project.pbxproj`.
4. Record the commit, build configuration, UTC start time, iPhone model/iOS version, Windows version, executable used, connection type, and consumer used. Do not infer any of them.
5. Treat Mac/iPhone and Windows as separate hosts. A Mac agent cannot claim Windows evidence it did not receive, and a Windows agent cannot verify iOS capture or encoding.

## Choose the Windows session mode

Determine this before launching the phone:

- `RemoteCam.exe` is the production path. Until pairing/authentication is implemented, it intentionally sends `server_info {paired:false}`, withholds `ready`, and may close the session. This proves discovery/TCP/`hello` only; it cannot prove capture or video.
- `RemoteCam-E2E.exe` or `rc-fakepc --allow-insecure-session` is the explicit Debug walking-skeleton path. Launch the iOS Debug app with `--allow-insecure-session` only against one of these test-only hosts.
- Never add an insecure trust bypass to a shipping target to make a hardware test pass.

State the selected mode in the saved sample. A production pairing stop is an expected implementation boundary, not a camera failure.

## Prepare the iPhone capture

1. Confirm a physical device and the installed app:

   ```sh
   xcrun devicectl list devices
   xcrun devicectl device info apps --device <device-id>
   ```

2. Build and test before installing a modified app. Use an available simulator identifier rather than assuming a simulator name:

   ```sh
   xcodebuild -project ios/RemoteCam.xcodeproj -scheme RemoteCam \
     -destination 'platform=iOS Simulator,id=<simulator-id>' test
   xcodebuild -project ios/RemoteCam.xcodeproj -scheme RemoteCam \
     -configuration Debug -destination 'platform=iOS,id=<physical-udid>' \
     -derivedDataPath /tmp/remotecam-device-build build
   xcrun devicectl device install app --device <device-id> \
     /tmp/remotecam-device-build/Build/Products/Debug-iphoneos/RemoteCam.app
   ```

3. Create a task-specific capture directory outside the repository. Launch through `devicectl --console`; this arms the monitor and launches the phone app, so tell the user not to launch a second instance:

   ```sh
   capture_dir="$(mktemp -d /tmp/remotecam-device-capture.XXXXXX)"
   xcrun devicectl device process launch --device <device-id> \
     --terminate-existing --console org.remotecam.ios \
     [--allow-insecure-session] 2>&1 | tee "$capture_dir/ios-console.raw.log"
   ```

   Keep the process in an interactive execution session so it can be stopped after the user says the capture is complete. `devicectl --log-output` records devicectl activity but may omit the app's bridged stdout; do not rely on it as the only capture.

4. After the monitor reports app startup and Bonjour readiness, tell the user exactly what to launch on Windows and what to tap on the phone. Do not leave a live capture without an update for more than 60 seconds.

## Collect Windows evidence

Capture Windows evidence on the Windows host for the same interval:

- App log: `%ProgramData%\RemoteCam\logs\rc-app.log` and rotated `.log.1`.
- Session 0 virtual-camera log: `%ProgramData%\RemoteCam\logs\rc-vcam.log` and rotated `.log.1`.
- Debug host/probe stdout when using `rc-fakepc`, `RemoteCam-E2E`, `rc-vcam-probe`, or `rc-fakewriter`.
- The Windows UI state and exact executable/configuration used.

Use a task-specific temporary directory. Copy the existing generations before the run, then copy them again after it; use `Get-Content -Wait` or `Tee-Object` only when live Windows monitoring is useful. Do not assume the Mac can read `%ProgramData%` remotely.

The Frame Server loads `rc-vcam.dll` as LOCAL SERVICE in Session 0. For black/missing virtual-camera video, inspect `rc-vcam.log` before changing code. Opening a consumer creates the `Global\` frame ring; the producer cannot prove that path while no consumer has RemoteCam selected.

## Exercise boundaries in order

Run only the phases relevant to the user's request. Record user actions and approximate timestamps.

1. **Startup:** camera and local-network authorization; camera enumeration; app/discovery services start once.
2. **Discovery:** Bonjour browser ready; valid `_remotecam._tcp` result. If absent, try manual IP without treating that fallback as a discovery pass.
3. **TCP:** iOS `NWConnection` ready and Windows listener acceptance.
4. **Protocol:** phone sends `hello`; PC returns `server_info`.
5. **Trust:** pairing succeeds or the documented production pairing gate is reached.
6. **Capture:** PC sends authenticated/test-only `ready`; camera configuration commits; `AVCaptureSession` reports running.
7. **Encode/transport:** VideoToolbox becomes ready; access-unit counts rise; keyframes are sent; no sustained queue-budget drops.
8. **Receive/decode:** Windows records incoming video/keyframes, decode progress, queue depth, and recovery behavior.
9. **Frame ring:** a consumer opens RemoteCam; `rc-vcam.log` shows the Session 0 source/ring; the producer transitions to publishing.
10. **Consumer:** the selected Windows application displays changing video rather than a stale or placeholder frame.

Only after the baseline passes, and only when requested, exercise Wi-Fi loss/reconnect, camera/lens controls, screen lock/background capture, format changes, thermal duration, MF/DirectShow probes, or the consumer matrix.

## Interpret evidence by boundary

Report the last confirmed boundary and the first unconfirmed boundary:

| Evidence | Proves | Does not prove |
|---|---|---|
| Bonjour host listed | discovery metadata reached iOS | TCP listener or video |
| Manual TCP `ready` state | network route and listener | Bonjour discovery |
| `hello` / `server_info` | framing and initial control exchange | pairing or capture |
| `awaiting_pairing` | production trust gate worked | camera failure |
| camera authorization + enumeration | permission and AVFoundation discovery | capture running |
| `AVCaptureSession running=true` | live capture session start | encoded output |
| encoded access-unit counters | VideoToolbox output | Windows received/decoded it |
| Windows decode counters | receiver/decode path | frame-ring/consumer display |
| `rc-vcam.log` plus publishing transition | Session 0 handoff | third-party UI correctness |
| changing consumer image | end-to-end display for that consumer | the full compatibility matrix |

Useful isolation patterns:

- Bonjour finds zero hosts but manual TCP reaches `server_info`: investigate Windows DNS-SD advertisement/network discovery, not the protocol listener.
- Repeated reset after `server_info {paired:false}` on the production target: record the pairing boundary; do not expect camera setup or video from that session.
- iOS sends keyframes but Windows has no receive evidence: investigate transport/receiver logging before AVFoundation or VideoToolbox.
- Windows publishes but the consumer is black: inspect `rc-vcam.log`, ring geometry/generation, and consumer selection before changing iOS.

## Add logs without creating a new failure

Use the existing iOS `RemoteCamLog` path and Windows `RC_LOG`/`RC_WARN`/`RC_ERR` paths.

- Log transitions, configuration, message type/channel, sizes, counters, OSStatus/HRESULT, reconnect attempt/delay, queue drops, and periodic summaries.
- Do not log frame contents, control payloads, pairing codes, keys, tokens, stable device IDs, or full per-frame events.
- Keep frame-path warnings edge-triggered. Windows logs rotate at 4 MiB; high-frequency logs can erase the failure being investigated.
- Emit Debug stdout on iOS when `devicectl --console` is the capture path, while retaining Unified Logging for normal diagnostics.
- Rebuild, rerun simulator tests, build the physical-device target, install, and observe the changed boundary before claiming a logging or lifecycle fix works.
- Do not reset privacy permissions, delete app data, alter pairing stores, or unregister a virtual camera unless the user authorized that state change.

## Stop, sanitize, and save the sample

1. On “capture complete,” stop the interactive console capture promptly. Note that forwarding `Ctrl+C` through `devicectl --console` terminates the instrumented app.
2. Preserve complete raw files only in the temporary capture directory unless the user explicitly requests secure archival.
3. Redact before Git:
   - IP addresses, hostnames, device names, stable IDs/UDIDs/UUIDs;
   - usernames, email addresses, development team/provisioning identifiers, and absolute user paths;
   - pairing codes, cryptographic material, tokens, and credentials.
4. Preserve diagnostic facts: timestamps or relative timing, device model/OS, build mode, port numbers, protocol message types, state transitions, byte/count metrics, retry delays, OSStatus/HRESULT/POSIX codes, and redaction markers.
5. Save a concise representative sample at `docs/testing/log-samples/YYYY-MM-DD-<scope>.log`. Start it with comment lines listing capture date, commit, devices/OS, Windows mode, connection type, sources included/missing, and sanitization. Include a short repeated cycle once rather than dozens of identical reconnects.
6. Update the existing status section in `ios/CLAUDE.md`, `windows/CLAUDE.md`, or root `CLAUDE.md` only with hardware steps actually observed. Never turn a build or seam test into a device-verification claim.

## Diagnose and hand off

Lead with the outcome, then state:

- last confirmed boundary;
- first unconfirmed boundary;
- expected versus unexpected behavior;
- evidence from both hosts, clearly labeling any missing side;
- smallest next test or authorized fix.

If behavioral code was changed, distinguish the original capture from the post-fix verification in the sample. Do not overwrite evidence to make a fix appear part of the original run.

## Publish only when authorized

When the user asks for GitHub publication, use the available GitHub publishing workflow. Inspect the full diff, create a `codex/` branch from the default branch, stage explicit paths, commit tersely, push, and open a draft PR unless the user requested a different flow.

Never stage raw unsanitized captures, derived build products, personal signing/team changes, unrelated worktree changes, or credentials. Run `git diff --check` and the relevant tests before pushing. Include the sanitized sample and the exact verification performed in the PR description.
