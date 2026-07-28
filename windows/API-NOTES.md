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

**Until then:** the app must ensure it is the only writer. A single-instance mutex in
the Qt client covers the realistic case.

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

## Include-order constraint

`ks.h` macro-defines `GUID_NULL`. Any translation unit that includes it **must** include
the COM and Media Foundation headers first, or `cguid.h` fails to compile from inside
the Windows SDK. `windows/vcam/media_source.h` has the working order; copy it rather
than rediscovering this.
