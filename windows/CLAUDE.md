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
**10.0.26100.0**, CMake 3.26.3, Ninja 1.11.1. Qt 6 and a linkable FFmpeg are not
installed and **M1 needs neither** — the virtual camera, its tools and its tests build
with nothing beyond the SDK.

```sh
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

Binaries land in `build/bin/` so `rc-vcam-register.exe` finds `rc-vcam.dll` beside
itself.

## Targets, in build order

| Target | State | What |
|---|---|---|
| `windows/common/` | **built** | `rcwin-common` — Win32 helpers, logging, NV12 geometry, the test pattern, the `Global\` frame ring. Plus `rcwin-common-tests`. |
| `windows/vcam/` | **built** | `rc-vcam.dll` — MF media source COM server. Loaded by the Frame Server in Session 0, **not** by our app. |
| `windows/register/` | **built** | `rc-vcam-register.exe` — elevated one-shot registration. Install time only. |
| `windows/tools/` | **built** | `rc-vcam-probe` (opens the vcam via MF *and* DirectShow, dumps frames), `rc-fakewriter` (publishes into the ring). `rc-fakephone` still to come. |
| `windows/platform/` | not started | D3D11 pipeline, FFmpeg `d3d11va` decode, effects, SHM writer. Everything platform-bound that `core/` may not touch. |
| `windows/app/` | not started | Qt 6 / QML client, tray, hotkeys. |
| `windows/obs-plugin/` | not started | Reads the same SHM ring directly, skipping the vcam round-trip. |
| `windows/installer/` | not started | WiX MSI. |

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
