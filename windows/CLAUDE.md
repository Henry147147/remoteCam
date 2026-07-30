# Windows agent

Owns `windows/`. Read the root [CLAUDE.md](../CLAUDE.md) first — the locked
decisions and the "do not re-derive" facts there are load-bearing, especially the
Session 0 and `HKCR` gotchas.

[API-NOTES.md](API-NOTES.md) records the non-obvious contracts inside `windows/` and
the changes that are known to be needed but deliberately deferred. Read it before
changing anything in `windows/common/` — more than one component consumes it now.

## Toolchain

Windows 11 build 22000+, MSVC (VS 2022 or newer), CMake ≥ 3.21, Qt 6, Windows SDK
with `mfvirtualcamera.h` / `mfsensorgroup.lib`. FFmpeg built **without**
`--enable-gpl`. Optional: NVIDIA Maxine redistributable and an RTX card for the
Maxine path; the build must succeed and the app must run without either.

**x64 only.** The Frame Server is a 64-bit process and will never load a 32-bit
in-proc server; `windows/CMakeLists.txt` fails at configure time rather than letting
you produce a camera that registers and then silently never delivers a frame.

Verified working on the dev box: MSVC 19.44 (VS 2022 Build Tools), Windows SDK
**10.0.26100.0**, CMake 3.26.3, Ninja 1.11.1, dynamic Qt **6.8.3**, NSIS **3.12**,
and pinned LGPL FFmpeg **8.1.2** through the root `vcpkg.json`. M1 still needs neither
Qt nor FFmpeg; the app/package and optional decoder do.

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

`RC_BUILD_INSTALLER=ON` adds CPack + NSIS and produces
`RemoteCam-<version>-win64.exe`. It needs Qt 6.5+ and NSIS on PATH, and it refuses to
configure without the Qt app — a package with a camera and nothing to drive it is not
worth shipping. **Release only**: the debug CRT and debug Qt are not redistributable,
and both a configure-time and an install-time guard say so.

```sh
cmake -S . -B build -A x64 -DRC_BUILD_INSTALLER=ON -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/msvc2022_64
cmake --build build --config Release
cpack --config build/CPackConfig.cmake -C Release -B build
```

`cmake --install build --config Release --prefix stage` stages the same payload
without building the .exe, which is the fast way to check what Qt deployment produced.
The install layout is deliberately flat — one directory holding `RemoteCam.exe`, the
Qt runtime, `rc-vcam.dll` and `rc-vcam-register.exe` — because the register tool
resolves the DLL next to itself and Qt resolves `platforms/` and its QML modules
relative to the executable.

The installer runs `rc-vcam-register.exe --register` on install and `--unregister` on
uninstall. The NSIS template already declares `RequestExecutionLevel admin`, which is
what makes that work; the register tool is `asInvoker` and will not self-elevate.

CI is configured to build this on every push to `main` and attach it to a GitHub
Release on `v*` tags. A hosted runner cannot install a system-wide virtual camera and
open a real consumer. The v0.1.0 installer has been generated locally, its NSIS
archive passed an integrity test, and its staged Qt payload passed a startup smoke
test. An elevated install, camera registration, upgrade/uninstall, and real-consumer
pass remain human steps; until those run, it is not system-verified.

## Targets, in build order

| Target | State | What |
|---|---|---|
| `windows/common/` | **built** | `rcwin-common` — Win32 helpers, logging, NV12 geometry, the test pattern, the `Global\` frame ring. Plus `rcwin-common-tests`. |
| `windows/vcam/` | **built** | `rc-vcam.dll` — MF media source COM server. Loaded by the Frame Server in Session 0, **not** by our app. |
| `windows/register/` | **built** | `rc-vcam-register.exe` — elevated one-shot registration. Install time only. |
| `windows/tools/` | **built and tested** | `rc-vcam-probe`, `rc-fakewriter`, explicit opt-in Debug `rc-fakepc`, and non-shipping `rc-fakephone` with stateful scenarios, replay, PCG32 chaos, NDJSON/JUnit, and real-loopback tests. |
| `windows/net/` | **built and tested** | Bounded dual-stack framed TCP listener, reusable resolving client, one phone at a time, serialized sends, `TCP_NODELAY`, clean stop, and inbox Windows DNS-SD registration. |
| `windows/backend/` | **built and tested** | Auth-gated session controller, hello/progress/idle timeouts, bounded 8-AU/20-MiB queue, recovery, metrics, observer and encoded-consumer seams. No insecure policy implementation lives in this library. |
| `windows/platform/` | **built and seam-tested** | D3D11VA FFmpeg decoder factory, NV12 D3D11 transform, ordered/PTS-preserving pipeline, and ABR. Hardware decode and the live sink are not verified. |
| `windows/app/` | **built and native-state-tested** | `RemoteCam.exe` plus non-installed `RemoteCam-E2E.exe`; stable QML automation IDs and native UIA/PrintWindow evidence. Production reports unpaired and withholds `ready`. Live preview, full controls, tray, hotkeys, secure session, and live sink integration remain, and the desktop harness reports them missing. |
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

```bash
build\bin\rc-vcam-probe.exe --directshow --frames 60
```

The probe asserts two independent properties, because they fail for unrelated reasons
and look identical in a preview window: the pattern's **static region** must hash the
same on every frame (proves stride, plane offsets and colour range are right) and its
**moving region** must differ on every frame (proves the camera is live rather than
repeating one frame). Both properties are also covered by `rcwin-common-tests`, so a
probe failure indicts the camera path rather than the generator.

Finally, the actual Session 0 proof: open a consumer, confirm the "WAITING FOR PHONE"
placeholder, then run `build\bin\rc-fakewriter.exe`. The picture must switch to the
"SHM WRITER" pattern within ~250 ms and revert when the writer is killed.

**If that last step fails**, the fallbacks in PLAN.md are: a small always-on broker
service that owns the section, or `MFVirtualCameraAccess_CurrentUser`. Escalate before
working around it — this decision shapes everything downstream.

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
