# Windows API notes

Contracts in `windows/` that are not obvious from the signatures, and changes that are
known to be needed but have been **deliberately deferred** rather than made.

This file exists because `windows/` is now consumed by more than one thing at a time —
`rc-vcam.dll`, the tools, and soon the Qt app and the OBS plugin — so a contract that
lives only in one caller's head is a contract that will be broken. Anything here that
says "deferred" is a real change someone will eventually have to make; make it
deliberately, in its own commit, and update this file.

Nothing in this file affects `core/` or `docs/protocol.md`. Those are the surfaces
shared with the iOS side, and they are unchanged.

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

## Deferred: single-producer enforcement

`writeFrame` advances `writeSeq` with a plain read-modify-write. Two processes
publishing concurrently will interleave into the same slot and corrupt it, silently —
the seqlock protects readers from a *writer*, not writers from each other.

Today this is a documented invariant with nothing enforcing it. Running two
`rc-fakewriter` instances is enough to violate it.

Considered and rejected for now:

- **`InterlockedIncrement` on `writeSeq`.** Cheapest, and wrong: the sequence has to be
  published *after* the pixels land, so making the increment atomic does not stop two
  writers picking the same slot and memcpy-ing over each other.
- **A named mutex around the write.** Correct, but the mutex would have to live in the
  `Global\` namespace and therefore be created by the DLL, which drags the privilege
  problem into a second object and adds a cross-session acquire to every frame.
- **An `ownerPid` claim field in the header.** Cheap and lock-free, but a producer that
  crashes never releases its claim, so it needs liveness detection (open a handle to
  the recorded pid and check it) to avoid wedging the ring until reboot.

The `ownerPid` approach is the likely answer. It would change `open()`'s contract — a
second writer would start failing where it currently succeeds — so it belongs in its
own commit, with `open()`'s documentation updated and a test that a crashed producer
does not lock the ring permanently.

**Until then, each producer guards itself.** `rc-fakewriter` holds
`Local\RemoteCam.FakeWriter.Single` and refuses to start twice, which covers the one
way anyone is realistically going to hit this. The Qt client will need the same when it
lands. This is a guard per producer, not enforcement in the ring — two *different*
producers would still collide.

---

## Deferred: geometry negotiation

`rc-vcam.dll` advertises exactly one media type (NV12 1920×1080 @ 30) and
`FrameSource::fill` discards any ring frame whose dimensions do not match, logging a
warning. That is correct for M1 and untenable once the format ladder from PLAN.md §1
lands: the consumer picks the resolution, and the producer currently has no way to
learn what was picked.

The ring header already carries `width`/`height`/`stride`/`formatGeneration`, but they
are written by the *producer* and read by the consumer — the wrong direction for this.
Negotiation needs the DLL to publish the requested geometry and the producer to read
it, which is a new field group and a new contract, not a tweak.

Do not work around this by having the producer guess, or by scaling in `FrameSource` —
the transform and scaling belong in the D3D11 pipeline (M2), on the producer side,
where `rc::transform`'s matrix is already available.

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

The Qt app advertises `_remotecam._tcp.local` with the inbox Windows
`DnsServiceRegister` API. It deliberately uses multicast DNS
(`unicastEnabled = FALSE`); this matches iOS `NWBrowser` and does not require a Bonjour
redistributable or Apple's restricted multicast entitlement.

The current integration port is TCP **7890** (`BonjourAdvertiser::kDefaultPort`). The
manual IP/port screen on iOS remains available and must use the same listener port.
When the production Windows TCP receiver lands, it must bind that port (or pass its
replacement port into the advertiser in the same change) and set `TCP_NODELAY` on
accepted phone connections. Advertising a port and listening on a different one is a
discovery failure even though both APIs report success.

TXT fields are the protocol-defined `v`, `name`, `id`, and `caps`. `id` is persisted
with `QSettings` under the current user and is exactly 16 lowercase hexadecimal
characters, so renaming the PC or changing its IP does not create a new pairing
identity. The registration is process-scoped; Windows removes it when the Qt app
exits. The DNS callback returns on an arbitrary thread and must marshal UI state back
to the Qt thread.

The installer/backend still needs an inbound Windows Firewall rule for the TCP
listener. DNS-SD can make the PC appear on the iPhone while a blocked TCP port makes
connection attempts time out, so these are separate manual verification checks.
