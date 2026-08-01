# RemoteCam wire protocol v1

The contract between the iPhone app and the Windows client. Both sides are written
against this document rather than against each other, so it is normative: if the
implementations disagree with this file, the file is right.

Status: **partially implemented**. The 16-byte framing, deterministic CBOR,
control/capability messages, and Annex-B validation are implemented in the portable
C++ core and iOS client with round-trip tests. Pairing, authentication, and media
encryption remain draft. Until they land, the only way to stream is the **mutual
unauthenticated opt-out** below, which both apps expose as a user setting; without it
the PC reports `paired:false` and withholds `ready`.

## Design constraints

Three constraints shaped everything below, and they are worth stating because they
rule out otherwise-obvious choices:

1. **Discovery must not use UDP broadcast or multicast.** Since iOS 14, sending to
   broadcast or multicast addresses requires Apple's
   `com.apple.developer.networking.multicast` entitlement, which is granted by
   application and review. Bonjour service browsing is exempt. So discovery is
   Bonjour, full stop.
2. **The same framing must work over USB.** The USB path is a byte stream through
   Apple's usbmux, not a datagram channel, so the protocol cannot depend on packet
   boundaries, MTU, or any UDP-only mechanism.
3. **Audio ships later but must not break the format.** v1 carries no audio, but the
   channel number is allocated now, so adding it is a capability flag rather than a
   version bump.

## Transport

One TCP connection per device, `TCP_NODELAY` set on both ends. The PC listens; the
phone connects. Identical over Wi-Fi and USB — only how the socket is obtained
differs.

### Wi-Fi

The PC publishes `_remotecam._tcp` with `DnsServiceRegister` (in `dnsapi.h`, part of
Windows — no Bonjour redistributable needed). The phone browses with `NWBrowser`.

TXT records:

| Key | Value | Meaning |
|---|---|---|
| `v` | `1` | protocol version |
| `name` | UTF-8 | PC display name |
| `id` | 16 hex chars | stable PC identity, survives rename and IP change |
| `caps` | comma list | e.g. `h264,hevc,enc` |

Manual IP entry and a recent-connections list are always available. Bonjour fails on
guest networks with client isolation and on some enterprise Wi-Fi, and being unable
to fall back is the single most common support complaint for tools in this category.

### USB

Windows connects to Apple Mobile Device Service's usbmux listener on
`127.0.0.1:27015` and speaks the plist protocol (`ListDevices`, `Listen`, `Connect`)
to open a tunnel to a port the iOS app is listening on at `127.0.0.1`. Once the
tunnel is open the byte stream is identical to Wi-Fi.

If AMDS is not installed there is no USB path at all. Detect this at startup and say
so plainly — "install Apple Devices or iTunes for USB" — rather than letting USB
silently never appear.

## Framing

Every message on the connection:

```
 0               1               2               3
 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
+---------------------------------------------------------------+
|                        length (u32, BE)                        |   bytes of payload
+---------------+---------------+-------------------------------+
|  channel (u8) |   flags (u8)  |        reserved (u16)          |
+---------------+---------------+-------------------------------+
|                     pts_micros (u64, BE)                       |   capture time
+---------------------------------------------------------------+
|                          payload                               |
+---------------------------------------------------------------+
```

Header is 16 bytes. All integers big-endian. `length` counts payload only and must
not exceed 16 MiB — a frame larger than that is a bug or an attack, and the receiver
closes the connection.

| Channel | Name | Payload |
|---|---|---|
| 0 | control | CBOR map |
| 1 | video | one access unit, Annex-B |
| 2 | audio | **reserved, unused in v1** — receivers must ignore, not error |
| 3 | stats | CBOR map |

Flags:

| Bit | Meaning |
|---|---|
| 0 | keyframe (video only) |
| 1 | payload is encrypted |
| 2 | end of a fragmented message |
| 3–7 | reserved, must be zero |

The keyframe bit is valid only on channel 1 and must agree with the access unit's VCL
NAL type. v1 does not fragment at this layer, so a receiver closes the connection if
bit 2 is present. The encryption bit is valid only after media encryption has been
explicitly negotiated; an implementation with no negotiated crypto must close rather
than feed ciphertext to a CBOR or video parser. Audio remains the one reserved v1
channel and is ignored without counting as progress. Other unknown channels have no
defined channel key or authenticated schema and close the session.

`pts_micros` is the capture timestamp on the phone's monotonic clock. The PC never
treats it as wall time; it is used for pacing and for the latency meter, both of
which only need differences.

All integer and length arguments in v1 CBOR use their shortest encoding. Known v1
message types reject fields outside their listed schema, including nested camera and
format maps. An unknown message type may be ignored as an additive extension only
after its complete session envelope has authenticated successfully; it never extends
a deadline. There is no unauthenticated extension namespace.

## Handshake

```
phone → PC   HELLO      {v, device_name, device_id, platform, model, caps,
                         allow_unauthenticated: bool}
PC   → phone SERVER_INFO{v, name, id, caps, paired: bool,
                         allow_unauthenticated: bool}
             ── if both allow_unauthenticated are true ──
PC   → phone READY       {stream_config}      (no pairing, no authentication)
phone → PC   STREAM_START
             ── otherwise, if not paired ──
PC   → phone PAIR_REQUIRED {salt: bstr16, pB: bstr65, expires}
             (PC shows a 6-digit code and a QR encoding host, port and code)
phone → PC   PAIR_COMMIT  {pA: bstr65}
PC   → phone PAIR_CONFIRM {macB: bstr32}
phone → PC   PAIR_VERIFY  {macA: bstr32}
             ── both derive and persist a long-term key ──
PC   → phone AUTH_CHALLENGE {server_nonce: bstr32, expires}
phone → PC   AUTH_RESPONSE  {client_nonce: bstr32, client_proof: bstr32}
PC   → phone AUTH_CONFIRM   {server_proof: bstr32, session_expires}
PC   → phone READY       {stream_config}
phone → PC   STREAM_START
```

`HELLO.v` must equal `1`; v1 does not treat `0` as a compatible subset. `device_id`
and the PC's advertised `id` are exactly 16 lowercase hexadecimal characters. This
canonical form is also the pairing-record key, so alternate case, prefixes, shorter
forms, and non-hex identifiers are rejected rather than aliased.

Pairing uses the exact SPAKE2 profile below over the 6-digit code, so the short code
never crosses the wire and cannot be brute-forced offline. The long-term key is stored
in the iOS Keychain and via DPAPI CurrentUser on Windows until explicit unpair; it has
no time-based re-pair expiry. Reconnects perform the nonce-bound authentication below
before `READY`.

An unpaired device may complete `HELLO` and nothing else. It cannot stream, cannot
read settings, and cannot enumerate anything about the PC beyond what the Bonjour
TXT record already broadcasts.

### The mutual unauthenticated opt-out

`allow_unauthenticated` is the one exception to the paragraph above, and it is a
deliberate user-selectable downgrade rather than a protocol weakness to be fixed.

Both peers expose it as a setting. `HELLO.allow_unauthenticated` states that the phone
is willing; `SERVER_INFO.allow_unauthenticated` states that the PC is. The PC sends
`READY` immediately after `SERVER_INFO`, skipping pairing and authentication entirely,
**only when both are true**. Neither flag grants anything on its own:

- A phone setting the flag is an unauthenticated peer asking to be believed. A PC whose
  own setting is off must ignore it and hold the phone at the pairing gate.
- A PC setting the flag is an offer. A phone that did not ask must refuse a `READY` that
  arrives without pairing, exactly as it would today.

The field is absent-tolerant in both directions: a peer built before this existed omits
it, which reads as `false`. A non-boolean value also reads as `false`. Both defaults
withhold the downgrade, so no malformed or outdated message can open a session.

Such a session has **no** control-channel HMAC and **no** media encryption. It carries
none of the guarantees the rest of this section describes, and both apps must say so in
their UI while it is active rather than presenting it as a normal connection. `paired`
remains `false` throughout: it reports whether a stored pairing record authenticated
this device, which is independent of, and never implied by, this opt-out.

### Normative pairing and session cryptography

SPAKE2 follows [RFC 9382](https://www.rfc-editor.org/rfc/rfc9382.html), ciphersuite
P-256/SHA-256. The phone/client is role A and uses M; the PC/server is role B and uses
N. M is `02886e2f97ace46e55ba9dd7242579f2993b64e16ef3dcab95afd497333d8fa12f`
and N is `03d8bbd6c639c62937b04d997f38c3770719c629d7014d49a24b4f98baa1292b49`.
Those constants are compressed SEC1; `pA`, `pB`, and the shared point on the wire or in
the transcript are exactly 65-byte uncompressed SEC1 points. Receivers require prefix
`04`, a non-infinity point on P-256, and subgroup order validation.

Role-A identity is the phone's canonical `device_id`; role-B identity is the PC's
canonical service `id`. The displayed code is exactly six ASCII digits, including
leading zeroes. It is stretched with scrypt `N=32768, r=8, p=1, maxmem=64 MiB` and a
fresh 16-byte salt to 40 bytes, interpreted big-endian, reduced modulo the P-256 order,
and encoded as a 32-byte big-endian scalar `w`. Zero is rejected.

The RFC transcript is exactly
`len(A)||A||len(B)||B||len(pA)||pA||len(pB)||pB||len(K)||K||len(w)||w`, where every
`len` is an unsigned 8-byte little-endian integer. `SHA-256(TT) = Ke || Ka`, 16 bytes
each. The pairing AAD is ASCII `RemoteCam SPAKE2 pairing v1` followed by the 16-byte
salt. `HKDF-SHA-256(Ka, salt=nil, info="ConfirmationKeys"||AAD, L=32)` splits into
`KcA || KcB`; `macA = HMAC-SHA-256(KcA, TT)` and likewise for B. Each side persists
only after verifying the other role's MAC.

The 32-byte long-term key is
`HKDF-SHA-256(Ke, pairing_salt, "RemoteCam long-term pairing key v1" ||
len(device_id)||device_id||len(service_id)||service_id, 32)`, with the same 8-byte
little-endian lengths. Windows protects the complete versioned record with DPAPI
CurrentUser and an identity-bound optional-entropy string, then atomically replaces it
using a flushed same-directory temporary file. The v1 expiry sentinel is `UINT64_MAX`;
unpair deletes the record.

A claimed `device_id` only selects that record. The PC sends a fresh server nonce and
the phone sends a fresh client nonce. Client and server proofs are HMAC-SHA-256 under
the long-term key over their distinct ASCII domain (`RemoteCam client authentication
proof v1` or `RemoteCam server authentication proof v1`), followed by length-prefixed
device ID, length-prefixed service ID, server nonce, and client nonce. Only a verified
client proof grants trust. The challenge expires after 10 seconds.

Session keys use HKDF-SHA-256 with the long-term key, salt
`server_nonce||client_nonce`, and info `RemoteCam session keys v1` followed by the
same length-prefixed device and service IDs. The 216-byte result splits, in order,
into six 36-byte records: control A→B, control B→A, video A→B, video B→A, statistics
A→B, statistics B→A. Each record is a 32-byte key and a session-derived 4-byte nonce
prefix.

Authenticated control/statistics payloads are
`sequence_u64_be || plaintext || hmac_sha256`. The MAC input is the exact final
16-byte wire header, then the same sequence, ASCII `RemoteCam control envelope v1`,
then plaintext. Encrypted video/statistics payloads are
`sequence_u64_be || ciphertext || tag16`; nonce is the channel record's 4-byte prefix
followed by the 8-byte big-endian sequence, and AEAD AAD is the exact final wire header,
sequence, and ASCII `RemoteCam media envelope v1`. The header therefore authenticates
payload length, channel, final flags, zero reserved bytes, and PTS.

Sequences start at zero independently for every direction and channel. Replays, gaps,
and modified AAD fail without advancing the receiver. A session reconnects before
either 24 hours or `2^32` records on any direction/channel; there is no nonce wrap and
no recovery fallback. OpenSSL must be exactly 3.5.7 in the Windows production build;
an absent or different version compiles only a fail-closed unavailable implementation.

### Session state and deadlines

The receiver enforces the message order, not just the happy-path replies:

- `hello` is the first control message and appears exactly once.
- `stream_start` appears exactly once after `ready`; video before it is discarded.
- phone telemetry is accepted only after trust. PC-to-phone control types received in
  the opposite direction are a protocol failure.
- malformed individual CBOR messages are ignored, and an authenticated unknown
  additive message type is ignored, but neither extends a deadline. Known types with
  unexpected fields are malformed. Reserved audio is not activity; other unknown
  channels have no authenticated v1 schema and fail the session.

The current PC bounds an incomplete session with 5 s for `hello`, 10 s for an
authentication exchange, 10 s from `ready` to `stream_start`, 5 s for a
format acknowledgement, 5 s for first/subsequent valid video, and 15 s for other
authenticated activity. PAKE state expires after 2 minutes. Five failed or unfinished
attempts in 10 minutes, either from one source address or globally, trigger a 5-minute
cooldown; successful completion removes only that attempt's provisional charge.

### What encryption does and does not cover

Per project decision, **media encryption is a toggle, default off** — on a trusted
LAN the CPU and latency are better spent elsewhere.

In a **paired** session the control channel is **always** authenticated with an HMAC
keyed on the long-term pairing key, regardless of that toggle. That is not optional:
without it, anyone on the network could push control messages that retarget the camera,
change resolution, or start a recording. Authentication of control and confidentiality
of media are separate properties and only the second one is a user preference.

A session established through the mutual unauthenticated opt-out has no long-term key,
so it has neither property. The risk above is exactly what the user accepts by enabling
that option on both devices; it is not something the implementation can mitigate, which
is why both apps state it plainly rather than burying it.

When the media toggle is on, video (channel 1) and statistics (channel 3) are encrypted
with ChaCha20-Poly1305. With it off, video remains cleartext and statistics use the
authenticated envelope. Audio is reserved. Keys and nonce prefixes are distinct for
every direction and channel.

## Control messages

CBOR maps with a `t` (type) key. Each known type has an exact v1 field set. Unknown
types are the authenticated additive-extension policy described above.

**Phone → PC**

| `t` | Payload | Notes |
|---|---|---|
| `hello` | see handshake | `allow_unauthenticated` absent or non-boolean reads as `false` |
| `auth_response` | `{client_nonce: bstr32, client_proof: bstr32}` | cleartext final authentication request |
| `caps` | camera list, supported resolutions/fps, lens list | sent once after `ready` |
| `orientation` | `{deg, locked}` | drives auto-rotate on the PC |
| `camera_state` | current ISO, exposure, WB, focus, zoom, torch | echoed after every change |
| `thermal` | `{state}` | `nominal`/`fair`/`serious`/`critical` |
| `battery` | `{level, charging}` | |
| `error` | `{code, message}` | |
| `format_ack` | `{generation}` | sent after the requested encoder format is active |
| `format_reject` | `{generation, code, message}` | rebuild failed; keep the last committed format |

**PC → phone**

| `t` | Payload | Notes |
|---|---|---|
| `server_info` | see handshake | `paired` reports a stored pairing record; `allow_unauthenticated` is independent of it |
| `auth_challenge` | `{server_nonce: bstr32, expires}` | cleartext, 10-second TTL |
| `auth_confirm` | `{server_proof: bstr32, session_expires}` | final cleartext handshake message |
| `ready` | `{codec, width, height, fps, bitrate}` | |
| `set_camera` | `{lens, position}` | front/back, ultra-wide/wide/tele |
| `set_format` | `{codec, width, height, fps, bitrate, generation}` | live changes require an acknowledgement |
| `set_control` | any subset of ISO, exposure, WB, focus, zoom, torch, stabilization | absent keys mean "leave alone" |
| `request_keyframe` | `{}` | after a decoder reset |
| `set_preview` | `{enabled}` | phone stops rendering its own preview — the largest single battery saving available |

Orientation is reported, never obeyed: the phone tells the PC how it is being held
and the PC decides what to do. The user's manual rotation offset always composes on
top, and an orientation lock on the PC ignores the field entirely. Letting the phone
drive the output directly is what makes a mounted phone unusable the moment someone
nudges it.

## Video

- HEVC preferred, H.264 High as fallback, negotiated in `hello`/`ready`.
- Annex-B byte stream. Parameter sets (VPS/SPS/PPS) are prepended to every keyframe,
  before its first random-access slice, not sent once or appended afterwards — a
  receiver that joins or resets mid-stream must be able to decode from the next
  keyframe with no side channel. Every video message contains at least one VCL NAL.
- One access unit per message. No fragmentation at this layer; TCP handles it.
- Keyframe every ~2 s, plus on demand via `request_keyframe`.

The wire keyframe flag is a claim the receiver verifies, not a substitute for parsing:
flagged delta frames, unflagged random-access frames, parameter-set-only messages, and
keyframes whose parameter sets occur after the first random-access slice are dropped.
Any such failure returns the receiver to "waiting for keyframe" and triggers
`request_keyframe`.

### Live format changes

The initial `ready`/`stream_start` configuration is implicit generation 0. Every live
change uses a positive monotonically increasing generation:

```text
PC    → phone  set_format {codec, width, height, fps, bitrate, generation: N}
phone → PC     format_ack {generation: N}   # capture/encoder rebuild is complete
PC    → phone  request_keyframe {}
phone → PC     video generation N, beginning with a self-contained keyframe
```

After receiving `set_format`, the phone stops old-generation media, applies the new
format, and sends the matching acknowledgement before any new-generation media. The
PC gates media while the acknowledgement is pending, flushes its encoded queue, and
serializes decoder reset against in-flight decode before accepting generation N. A
missing or mismatched acknowledgement never commits the new codec and closes the
session on the format-acknowledgement deadline. This ordering prevents old queued H.264
from entering a newly rebuilt HEVC decoder (and the converse).

If rebuilding capture or the encoder fails, the phone instead sends
`format_reject {generation: N, code, message}`. The PC clears only that matching
pending request, retains its previously committed decoder configuration and stream
generation, requests a keyframe, and resumes the old format. A rejection with no
pending request or a mismatched generation is a protocol failure.

The PC decodes with FFmpeg's `d3d11va` hwaccel, which uses the GPU driver's DXVA
decoder. This matters for a specific reason: Media Foundation's own HEVC decoder
requires the user to buy **HEVC Video Extensions** from the Microsoft Store. Going
through DXVA avoids putting a paid dependency in a project whose whole point is that
nothing is paid.

## Stats and adaptive bitrate

Channel 3, PC → phone, ~2 Hz:

```
{t: "stats", queue_depth, decode_ms, drops, rtt_ms, target_bitrate}
```

The phone retunes `VTCompressionSession`'s average bitrate toward `target_bitrate`.
The PC computes it from queue depth and observed throughput: back off fast on
sustained queue growth, recover slowly. The phone never raises bitrate on its own.

## Latency budget

Target **≈45 ms glass-to-glass at 1080p60**, apportioned:

| Stage | Budget |
|---|---|
| capture | 8 ms |
| encode | 12 ms |
| network | 5 ms |
| decode | 8 ms |
| transform + effects | 5 ms |
| virtual camera handoff | 5 ms |

Measured, not assumed: `pts_micros` rides through the whole pipeline to the virtual
camera sink, and the diagnostics page reports the real distribution. Any stage that
consistently exceeds its budget is a bug against this table.

## Versioning

`v` is a single integer. A v1 receiver requires exactly `1`; a different value is
refused rather than guessed at. Additive changes — new control
message types, new keys, new capability flags — do not bump it; the ignore-unknown
rules above cover them.
