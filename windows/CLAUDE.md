# Windows agent

Owns `windows/`. Read the root [CLAUDE.md](../CLAUDE.md) first — the locked
decisions and the "do not re-derive" facts there are load-bearing, especially the
Session 0 and `HKCR` gotchas.

## Toolchain

Windows 11 build 22000+, MSVC (VS 2022 or newer), CMake ≥ 3.21, Qt 6, Windows SDK
with `mfvirtualcamera.h` / `mfsensorgroup.lib`. FFmpeg built **without**
`--enable-gpl`. Optional: NVIDIA Maxine redistributable and an RTX card for the
Maxine path; the build must succeed and the app must run without either.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=<qt6>
cmake --build build --config Debug -j
ctest --test-dir build -C Debug --output-on-failure
```

## Targets, in build order

| Target | What |
|---|---|
| `windows/vcam/` | `rc-vcam.dll` — MF media source COM server. Loaded by the Frame Server in Session 0, **not** by our app. |
| `windows/register/` | `rc-vcam-register.exe` — elevated one-shot registration. Install time only. |
| `windows/platform/` | D3D11 pipeline, FFmpeg `d3d11va` decode, effects, SHM writer. Everything platform-bound that `core/` may not touch. |
| `windows/app/` | Qt 6 / QML client, tray, hotkeys. |
| `windows/tools/` | `rc-fakephone` (replays a video file over the real protocol), `rc-vcam-probe` (opens the vcam via MF *and* DirectShow, dumps frames). |
| `windows/obs-plugin/` | Reads the same SHM ring directly, skipping the vcam round-trip. |
| `windows/installer/` | WiX MSI. |

## Start here — M1, the virtual camera

This is the highest-risk item in the project. Prototype it before building anything
on top of it. The order that de-risks fastest:

1. `rc-vcam.dll` producing a **static test pattern**, registered via
   `rc-vcam-register.exe`, verified with `rc-vcam-probe` through both MF and
   DirectShow. No networking, no pipeline, no Qt.
2. Then the `Global\` shared-memory ring and a trivial writer process, proving the
   Session 0 handoff.
3. Only then wire it to the real pipeline.

**If step 2 fails**, the fallbacks in PLAN.md are: run the frame producer as a
Windows service, or try `MFVirtualCameraAccess_CurrentUser` to see whether the source
stays in-session. Escalate before working around it — this decision shapes everything
downstream.

Ring buffer design is in PLAN.md §1: 4 NV12 slots, seqlock plus auto-reset event so
the writer never blocks, system-memory frames staged out of D3D11 with 3 rotating
staging textures and a fence.

**Always produce frames.** When no phone is connected, emit a placeholder — several
apps drop or error on a stalled device.

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
