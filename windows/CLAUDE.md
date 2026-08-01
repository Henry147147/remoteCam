# Windows agent

Owns `windows/`. Read the root [CLAUDE.md](../CLAUDE.md) first — the locked
decisions and the "do not re-derive" facts there are load-bearing, especially the
Session 0 and `HKCR` gotchas.

[API-NOTES.md](API-NOTES.md) records the non-obvious contracts inside `windows/` and
the changes that are known to be needed but deliberately deferred. Read it before
changing anything in `windows/common/` — more than one component consumes it now.

## Toolchain

Windows 11 build 22000+, MSVC (VS 2022 or newer), CMake ≥ 3.21 (≥ 3.26 for the
production preset), Qt 6, and a Windows SDK with `mfvirtualcamera.h` /
`mfsensorgroup.lib`. FFmpeg is built **without** `--enable-gpl`; OpenSSL 3 is a
dynamic production dependency. Optional: NVIDIA Maxine redistributable and an RTX
card for the Maxine path; the build must succeed and the app must run without either.

**x64 only.** The Frame Server is a 64-bit process and will never load a 32-bit
in-proc server; `windows/CMakeLists.txt` fails at configure time rather than letting
you produce a camera that registers and then silently never delivers a frame.

Verified working on the dev box: MSVC 19.44 (VS 2022 Build Tools), Windows SDK
**10.0.26100.0**, CMake 3.26.3, Ninja 1.11.1, dynamic Qt **6.8.3**, NSIS **3.12**,
pinned LGPL FFmpeg **8.1.2**, and pinned OpenSSL **3.5.7** through the root
`vcpkg.json`. M1 still needs none of Qt, FFmpeg, or OpenSSL; the complete app/package
does.

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

The optional hardware-decoder build uses the pinned manifest. vcpkg is configured
for dynamic x64 libraries so FFmpeg remains replaceable under the LGPL:

```powershell
cmake -S . -B build-ffmpeg -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DVCPKG_TARGET_TRIPLET=x64-windows -DRC_WITH_FFMPEG=ON
cmake --build build-ffmpeg --config Release --parallel
ctest --test-dir build-ffmpeg -C Release --output-on-failure
```

`RC_ENABLE_MSVC_ANALYZE=ON` adds `/analyze`; warning-as-error behavior is on by
default for every project target and can be disabled only with
`RC_WARNINGS_AS_ERRORS=OFF` during diagnosis.

Binaries land in `build/bin/` so `rc-vcam-register.exe` finds `rc-vcam.dll` beside
itself.

## Packaging

`windows-production` is the only shippable configuration. It combines the Qt app,
dynamic FFmpeg and OpenSSL runtimes, tests, and CPack/NSIS instead of validating each
piece in a mutually exclusive build. It needs dynamic Qt 6.5+, NSIS, and `VCPKG_ROOT`.
**Release only**: the debug CRT and debug Qt are not redistributable, and both a
configure-time and an install-time guard say so.

The manifest override and `vcpkg-overlays/openssl/` pin OpenSSL **3.5.7 exactly**.
The overlay verifies the official release archive and hashes every Windows support
file borrowed from the pinned vcpkg baseline before it builds; changing either the
OpenSSL version or the baseline is therefore an explicit release-maintenance task.

```powershell
$env:VCPKG_ROOT = 'C:\src\vcpkg'
$env:CMAKE_PREFIX_PATH = 'C:\Qt\6.8.3\msvc2022_64'
cmake --preset windows-production
cmake --build --preset windows-production
ctest --preset windows-production
cpack --config build-production/CPackConfig.cmake -C Release -B dist
```

Use an **absolute** stage prefix; Qt's deploy script rejects a relative one:

```powershell
$stage = Join-Path (Resolve-Path .) 'build-production\stage'
cmake --install build-production --config Release --prefix $stage
```

The install itself audits the payload: the app, camera components, FFmpeg, OpenSSL,
licences, Qt, and VC runtime must be present, while every test/fake/probe executable
must be absent. The layout is deliberately flat because the register tool resolves
the camera DLL beside itself and the dynamic runtimes must remain replaceable.

The installer and uninstaller request administrator elevation up front. Before any
file is written, setup rejects non-x64 Windows and builds older than Windows 11 22000.
That gate also runs before a previous version is removed; upgrades require a successful
checked uninstall and never overwrite an existing installation in place. Setup does not
launch `RemoteCam.exe` from its elevated finish page, so the desktop app always starts
later with the interactive user's normal token.
Camera registration and the private-network TCP 7890 firewall rule are transactional:
if either fails, setup runs the just-created uninstaller (with an explicit cleanup
fallback), preserves the failing exit code, and aborts. If camera cleanup itself fails,
setup deliberately retains the cleanup binaries and uninstall entry for a safe retry
instead of claiming success and orphaning the camera. Uninstall first checks that the
desktop executable and camera DLL are not locked, before it mutates camera, firewall,
file, or installer state. The generated NSIS delete block then stages the cleanup
helper and checks that critical payload really disappeared before deleting the
uninstaller or Add/Remove Programs metadata. Any failure retains that retry path,
shortcuts, and residual payload. Upgrade and failed-install rollback perform the same
critical-file check after an invoked uninstaller reports success; for an older
uninstaller that may already have removed its metadata, the caller promises only to
retain the remaining directory and in-place `Uninstall.exe`. No path recursively deletes
a user-selected install directory. Uninstall unregisters the camera, removes the
firewall rule through the helper's native Firewall COM path, deletes logs, and then
removes the payload. `VerifyNsisScript.cmake` statically audits those guarantees in CI
after CPack generates `project.nsi`, and
CI extracts the generated uninstaller from the final NSIS archive without running
setup, then `VerifyExecutionLevel.ps1` uses the Windows SDK manifest tool to inspect
RT_MANIFEST resource 1 in both executables and requires `requireAdministrator`.

CI builds, tests, lints, stages, smoke-starts, audits, and packages this preset on pull
requests, pushes, and `v*` tags. A tag must exactly match `project(RemoteCam VERSION)`.
When both `WINDOWS_SIGNING_PFX_BASE64` and `WINDOWS_SIGNING_PFX_PASSWORD` repository
secrets exist, CI signs and verifies the three project binaries and installer; when
either is absent, v1 is intentionally allowed to publish unsigned. Every installer
and release includes `SIGNING-STATUS.txt`, and that status is prepended to the release
notes so the unsigned case is explicit. CI then publishes the installer, SHA-256
checksum, SPDX SBOM, provenance attestation, and SBOM attestation. A hosted runner still cannot
install a system-wide virtual camera and open a real consumer. Elevated install,
camera registration, upgrade/uninstall, and real-consumer passes remain human release
steps; until they run, the release is not system-verified.

## Targets, in build order

| Target | State | What |
|---|---|---|
| `windows/common/` | **built** | `rcwin-common` — Win32 helpers, logging, NV12 geometry, the test pattern, the `Global\` frame ring. Plus `rcwin-common-tests`. |
| `windows/vcam/` | **built, automated format tests pass** | `rc-vcam.dll` — MF media source COM server with the fixed six-resolution × 30/60-fps NV12 ladder. Loaded by the Frame Server in Session 0, **not** by our app. |
| `windows/register/` | **built** | `rc-vcam-register.exe` — elevated one-shot camera registration and native Windows Firewall rule management. Install time only. |
| `windows/tools/` | **built and tested** | `rc-vcam-probe`, `rc-fakewriter`, explicit opt-in Debug `rc-fakepc`, and non-shipping `rc-fakephone` with stateful scenarios, replay, PCG32 chaos, NDJSON/JUnit, and real-loopback tests. |
| `windows/net/` | **built and tested** | Bounded dual-stack framed TCP listener, reusable resolving client, one phone at a time, serialized sends, `TCP_NODELAY`, clean stop, and inbox Windows DNS-SD registration. |
| `windows/backend/` | **built and tested** | Auth-gated session controller, hello/progress/idle timeouts, bounded 8-AU/20-MiB queue, recovery, metrics, observer and encoded-consumer seams. No insecure policy implementation lives in this library — `ITrustPolicy::allowsUnauthenticated()` only reports one, and `rcapp::SecurityPolicy` in `windows/app/` supplies it. |
| `windows/platform/` | **built and seam-tested** | D3D11VA FFmpeg decoder factory, NV12 D3D11 transform, ordered/PTS-preserving pipeline, and ABR. Hardware decode and the live sink are not verified. |
| `windows/app/` | **built and native-state-tested** | `RemoteCam.exe` plus non-installed `RemoteCam-E2E.exe`; stable QML automation IDs and native UIA/PrintWindow evidence. Production reports unpaired and withholds `ready` unless the user's "allow connecting without pairing" option and the phone's matching flag are both set. Live preview, full controls, tray, hotkeys, secure session, and live sink integration remain, and the desktop harness reports them missing. |
| `windows/obs-plugin/` | not started | Reads the same SHM ring directly, skipping the vcam round-trip. |
| `windows/installer/` | **packaged, elevated run unverified** | Self-contained CPack + NSIS installer with camera registration, private TCP 7890 firewall rule, shortcuts, upgrade/uninstall, licence and notices. |

## M1 — the virtual camera. Written; verification outstanding.

The code exists and compiles clean. **What has not happened is running it against a
real camera consumer**, and until that has been done M1 is not proven. Do this, in
order:

```bash
cmake --build build --config Debug
```

Then from an **elevated** prompt, once:

```bash
build\bin\rc-vcam-register.exe --register
```

Then, unelevated:

```bash
build\bin\rc-vcam-probe.exe --mf --frames 60
```

The MF probe enumerates and requires all 12 native NV12 types. Exercise selected-type
geometry and pacing explicitly as well; at minimum cover both rates and the largest,
default, and smallest canvases:

```bash
build\bin\rc-vcam-probe.exe --mf --format 3840x2160@30 --frames 60
build\bin\rc-vcam-probe.exe --mf --format 1920x1080@60 --frames 120
build\bin\rc-vcam-probe.exe --mf --format 640x480@30 --frames 60
build\bin\rc-vcam-probe.exe --mf --format 640x480@60 --frames 120
```

```bash
build\bin\rc-vcam-probe.exe --directshow --frames 60
```

The probe asserts two independent properties, because they fail for unrelated reasons
and look identical in a preview window: the pattern's **static region** must hash the
same on every frame (proves stride, plane offsets and colour range are right) and its
**moving region** must differ on every frame (proves the camera is live rather than
repeating one frame). Both properties are also covered by `rcwin-common-tests`, so a
probe failure indicts the camera path rather than the generator. It also checks the
negotiated timestamp interval against 30 or 60 fps; this catches a source that advertises
one rate while continuing to schedule the old default.

Finally, the actual Session 0 proof: open a consumer, confirm the "WAITING FOR PHONE"
placeholder, then run `build\bin\rc-fakewriter.exe`. The picture must switch to the
"SHM WRITER" pattern within ~250 ms and revert when the writer is killed.

**If that last step fails**, the fallbacks in PLAN.md are: a small always-on broker
service that owns the section, implemented behind `rcwin::IVirtualCameraBridge`, or
`MFVirtualCameraAccess_CurrentUser`. `Brokered` currently returns
`ERROR_NOT_SUPPORTED`; do not add a service before this physical proof fails. Escalate
before working around it — this decision shapes everything downstream.

`--session` creates a Session-lifetime camera that disappears when the process exits,
which makes it the right iteration loop after a rebuild. It still needs elevation:
`MFCreateVirtualCamera` returns `E_ACCESSDENIED` unelevated regardless of lifetime.

**Always produce frames.** When no phone is connected, emit a placeholder — several
apps drop or error on a stalled device.

### Debugging inside Session 0

There is no console, no message box anyone will see, and no debugger attached. Every
component logs to `%ProgramData%\RemoteCam\logs\` (and `OutputDebugStringW`); the
directory is created with a DACL granting LOCAL SERVICE write by
`rc-vcam-register.exe --register`. **Read `rc-vcam.log` first** on any "camera is
black" report — it is the only window into the Frame Server's copy of our code.

## Consuming `core/`

`rc::transform` is done and tested. `destToSource(params)` returns the row-major 3×3
backward map: for each destination pixel, sample the source at `M · p`. Feed it to
the shader as-is; do not recompute the matrix in HLSL.

Call `clampPanForCoverage` after every drag. Do **not** clamp `panX`/`panY`
independently — see the root CLAUDE.md correction on why that is geometrically wrong.

The transform runs **before** the AI effects. Maxine's segmentation and eye-contact
models expect an upright person; rotating afterwards feeds them a sideways face.

## Verification

Unit tests do not prove this component works. The virtual camera must be checked by
hand against real consumers every milestone:

**Zoom · Teams · Discord · Chrome (Meet) · OBS · Windows Camera**

Plus: rotate to 37° with Fill while streaming into Zoom — no black corners, no
dropped frames, correct aspect. On an RTX machine confirm the diagnostics page shows
Maxine active; on AMD/Intel confirm the same effects run via DirectML and Maxine
greys out with a reason rather than vanishing.

Report what you actually ran. "Builds clean" is not "works".
