# Windows API notes

Contracts in `windows/` that are not obvious from the signatures, including the
resolution of two changes that were previously deliberately deferred.

This file exists because `windows/` is now consumed by more than one thing at a time —
`rc-vcam.dll`, the tools, and the Qt app, with the OBS plugin still to come — so a
contract that lives only in one caller's head is a contract that will be broken.

This file documents the Windows bindings around the portable contracts in `core/` and
`docs/protocol.md`; those shared surfaces remain authoritative for wire behavior.

---

## `rcwin::FrameRing` — current contracts

**Creation is privileged; opening is not.** `create()` requires
`SeCreateGlobalPrivilege`, which only services and elevated processes hold, so it is
called by `rc-vcam.dll` inside the Frame Server. Producers call `open()`. This is
inverted from what PLAN.md originally described; see the third entry in
[PLAN.md's corrections](../PLAN.md#corrections-to-the-original-plan) for the
measurement that forced it.

**The ring only exists while a consumer has the camera open.** There is nothing to
`open()` until some application has caused Windows to load the DLL. Producers must poll
and tolerate the ring disappearing mid-run; `rc-fakewriter` shows the shape.

**Read-side validation, not write-side.** `writeFrame` records the geometry it is given
without checking it. `readLatest` validates before returning `S_OK`, and returns
`S_FALSE` for anything self-inconsistent.

This asymmetry is intentional and worth not "tidying up". The section's DACL grants
write access to `INTERACTIVE`, so a hostile producer would map it and write bytes
directly rather than politely calling `writeFrame` — validating on the write side would
therefore protect nothing. The read side is where untrusted data enters a process that
matters: `rc-vcam.dll` runs in `svchost.exe`, and an out-of-bounds read there takes the
Frame Server down for every camera application on the machine.

On `S_OK`, callers may rely on: even dimensions within `kRingMaxWidth`/`kRingMaxHeight`,
`stride >= width`, and `stride*height*3/2 <= bytesUsed <= kRingSlotBytes`.

**Exactly one producer.** See below.

---

## Single-producer enforcement — done, ring version 3

`writeFrame` advances `writeSeq` with a plain read-modify-write, so two processes
publishing concurrently would interleave into the same slot and corrupt it silently;
the seqlock protects readers from a *writer*, not writers from each other.

`open(writable)` now enforces it. The header carries an `ownerPid` claim plus the
claimant's process creation time, taken under the existing write guard. A second writer
gets `ERROR_ALREADY_EXISTS` rather than succeeding, and `close()` releases the claim.

Two details that are load-bearing:

- **The start time is not decoration.** Windows recycles pids. `ownerPid` alone would
  let an unrelated live process look like our dead producer and wedge the ring until
  reboot, which is exactly the liveness problem that kept this deferred.
- **There is no exception for our own pid.** Two `FrameRing` instances in one process
  are the same bug as two processes, and the shared header cannot tell them apart. A
  legitimate reopen has already released its claim in `open()`'s leading `close()`.

A claim we cannot inspect — a producer in another user's session, where `OpenProcess`
returns `ERROR_ACCESS_DENIED` — counts as **live**. Refusing to start is better than
two producers interleaving, which is invisible until someone looks at the video.

Crash recovery is tested for real: `testRingReclaimsCrashedProducer` spawns a child,
waits for it to claim the ring, `TerminateProcess`es it, and asserts a successor can
open. A simulated dead pid would only have tested the simulation.

`rc-fakewriter` keeps its `Local\RemoteCam.FakeWriter.Single` guard. It now produces a
better message than the ring's refusal would, rather than being the only thing standing
between two writers and a corrupt slot.

---

## Geometry negotiation — done, ring version 3

`FrameSource::fill` used to discard any ring frame whose dimensions did not match the
one advertised media type, log once, and leave the producer with no way to find out
what was wanted.

The header's `width`/`height`/`stride`/`formatGeneration` are written by the *producer*
and describe what it last sent — the wrong direction. Version 3 adds the other
direction: `requestedWidth`/`requestedHeight`/`requestedFormat`/`requestedGeneration`,
written by the consumer through `FrameRing::requestGeometry` and read by the producer
through `FrameRing::requestedGeometry`.

- **Consumer-only.** `requestGeometry` returns `E_ACCESSDENIED` for a producer; a
  producer writing it would be talking to itself.
- **Cleared on a new consumer generation.** The departing consumer's choice says
  nothing about what the arriving one wants, so `requestedGeneration` goes back to 0
  and the producer sees `S_FALSE` again.
- Media Foundation hands `FrameSource` the layout per frame, so that is where the
  negotiated geometry is actually known. It is published on change, not per frame.
- The producer validates any request before adopting it — an absurd geometry must not
  walk it off the end of a ring slot.

Still true, and still the rule: **scaling belongs in the D3D11 pipeline**, on the
producer side where `rc::transform`'s matrix already is. Negotiation tells the producer
what to render; it is not a licence to rescale inside `FrameSource`.

---

## Virtual-camera bridge topology — app-owned unless the physical gate fails

Producer code talks to `rcwin::IVirtualCameraBridge`, whose shipping implementation
opens the existing writable `FrameRing`. The virtual-camera DLL remains the consumer
that creates the `Global\` objects in Session 0; the unelevated desktop app remains
the only frame producer. This abstraction keeps that ownership rule explicit while
letting pipeline code avoid depending on shared-memory mechanics.

`VirtualCameraBridgeTopology::Brokered` deliberately returns
`ERROR_NOT_SUPPORTED`. It is an architecture boundary, not a dormant Windows service.
Do not add, install, or start a broker until the physical M1 gate has proved that an
unelevated app cannot open the Session 0 objects on a supported Windows build. If that
gate fails, implement the broker behind this interface so decoder and transform code
do not change topology along with it.

---

## Virtual-camera media types — fixed 12-type NV12 ladder

The source advertises six canvas sizes — 3840×2160, 2560×1440, 1920×1080,
1280×720, 960×540, and 640×480 — at both 30 and 60 fps. All types are progressive,
square-pixel, fixed-size NV12. The default is 1920×1080 at 30 fps; ordering it first
prevents a consumer that simply accepts the current type from accidentally choosing
4K60.

`IMFMediaSource::Start` reads the current media type from the presentation descriptor
the caller passes in. The selected width and height drive the 2D sample allocation,
NV12 layout validation, and frame-ring geometry request; the selected frame rate
drives sample duration and the high-resolution timer schedule. There must be no fixed
1080p30 buffer or pacing constant outside `media_format.h`.

`rc-vcam-format-tests` checks every advertised attribute and round-trips every type.
The live `rc-vcam-probe --mf` path independently enumerates the camera and fails if any
of the 12 native types is missing. `--format WxH@FPS` then selects an exact MF type and
also checks delivered timestamp pacing. These automated checks complement, but cannot
replace, the elevated registration and Session 0 physical gate.

---

## `rcvcam::MediaStream` — request queue is bounded

`RequestSample` keeps at most `kMaxPendingRequests` (8) outstanding requests and drops
the oldest beyond that, warning once per streaming session.

This is a deliberate departure from a strict reading of the Media Foundation contract,
where every `RequestSample` is answered. An unbounded queue is unbounded memory growth
inside a shared service driven by an external caller, which is the worse failure: a
consumer that over-requests loses a frame, where before it could have grown the Frame
Server's working set without limit. If a real consumer is ever observed to depend on
one-for-one delivery, raise the cap rather than removing it.

---

## Everything in the DLL must hold a `ModuleLock`

`DllCanUnloadNow` returning `S_OK` is a promise to COM that no code in `rc-vcam.dll` is
still executing, and COM acts on it by unmapping the module. Any object that lives in
the DLL but is not counted makes that promise a lie, and the Frame Server will unload
the code a running thread is currently in — an access violation inside `svchost.exe`
with no RemoteCam frame on the stack, because the frames belong to a module that no
longer exists.

`MediaSource`, `MediaStream` and the class factory each hold a `rcvcam::ModuleLock`
declared as their **first** member, so it is constructed first and destroyed last.
Anything new that is allocated here and handed across the COM boundary must do the
same. See `windows/vcam/module_lock.h`.

## Frame-ring lifetime is explicit

Ring version 2 adds a camera-consumer count, generation, and named write guard. A
producer's mapping handle keeps the kernel object alive after the virtual camera
closes, so handle validity alone cannot mean “connected.” `FrameRing::writeFrame`
now fails when its consumer generation disappears or changes; the Qt producer must
close that stale handle and return to polling. The write guard uses a zero timeout on
the frame path, preserving the non-blocking producer contract while serializing the
rare close/reopen transition.

## Logs are capped and rotated

`%ProgramData%\RemoteCam\logs\<tag>.log` is capped at 4 MB with one previous generation
kept as `<tag>.log.1`. The DLL is loaded by a service that can stay up for weeks, so
per-frame logging is a disk-consumption bug rather than a nuisance: warnings on any
per-frame path must be edge-triggered, logging on the transition and not on every
frame. `FrameRing::readLatest` and `FrameSource::fill` both do this.

## Include-order constraint

`ks.h` macro-defines `GUID_NULL`. Any translation unit that includes it **must** include
the COM and Media Foundation headers first, or `cguid.h` fails to compile from inside
the Windows SDK. `windows/vcam/media_source.h` has the working order; copy it rather
than rediscovering this.

## Bonjour advertisement and the Windows transport port

The Qt app can advertise `_remotecam._tcp.local` with the inbox Windows
`DnsServiceRegister` API. It deliberately uses multicast DNS
(`unicastEnabled = FALSE`); this matches iOS `NWBrowser` and does not require a Bonjour
redistributable or Apple's restricted multicast entitlement.

The current integration port is TCP **7890** (`BonjourAdvertiser::kDefaultPort`). The
manual IP/port screen on iOS remains available and must use the same listener port.
The Qt app starts `TcpListener` first and calls `BonjourAdvertiser::start()` only
after `listen()` succeeds, so it never advertises a closed endpoint. The listener
allows one phone at a time, sets `TCP_NODELAY`, bounds framing through `rc::wire`, and
closes malformed sessions. A production connection receives `server_info {paired:false}`
and no `ready` unless both peers set `allow_unauthenticated`; pairing/authentication is
otherwise still a hard gate.

**The SRV target must end in `.local`.** `DnsServiceConstructInstance`'s `pHostName` is
published verbatim, and Windows accepts a bare computer name without complaint:
registration succeeds, the UI says "advertising", and a Windows browser finds the
service because browsing reads only the PTR record. iOS then cannot resolve the SRV
target — it has no LLMNR or NetBIOS — so the phone silently never lists the PC, which
looks identical to advertising being broken. Measured on this repo before the fix:
`SRV HENRYDESKTOP._remotecam._tcp.local -> host='HENRYDESKTOP' port=7890`. Both
`BonjourAdvertiser` and `rcnet::BonjourService` now append `.local`. To re-measure
without an iPhone, run `rc-fakephone discover` for the PTR and query the SRV directly;
Windows answers per-interface, so ask from the LAN adapter, not the WSL vSwitch.

TXT fields are the protocol-defined `v`, `name`, `id`, and `caps`. `id` is persisted
with `QSettings` under the current user and is exactly 16 lowercase hexadecimal
characters, so renaming the PC or changing its IP does not create a new pairing
identity. The registration is process-scoped; Windows removes it when the Qt app
exits. The DNS callback returns on an arbitrary thread and must marshal UI state back
to the Qt thread.

The installer adds an inbound TCP 7890 rule scoped to the private firewall profile
and `RemoteCam.exe`, and removes that exact rule during uninstall. DNS-SD visibility
and TCP reachability remain separate physical-device verification checks.
