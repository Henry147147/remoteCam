# RemoteCam wire protocol v1

The contract between the iPhone app and the Windows client. Both sides are written
against this document rather than against each other, so it is normative: if the
implementations disagree with this file, the file is right.

Status: **partially implemented**. The 16-byte framing, deterministic CBOR,
control/capability messages, and Annex-B validation are implemented in the portable
C++ core and iOS client with round-trip tests. Pairing, authentication, and media
encryption remain draft; the production Windows app therefore reports `paired:false`
and withholds `ready` instead of starting an insecure session.

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

`pts_micros` is the capture timestamp on the phone's monotonic clock. The PC never
treats it as wall time; it is used for pacing and for the latency meter, both of
which only need differences.

## Handshake

```
phone → PC   HELLO      {v, device_name, device_id, platform, model, caps}
PC   → phone SERVER_INFO{v, name, id, caps, paired: bool}
             ── if not paired ──
PC   → phone PAIR_REQUIRED {salt}
             (PC shows a 6-digit code and a QR encoding host, port and code)
phone → PC   PAIR_COMMIT  {SPAKE2 message}
PC   → phone PAIR_CONFIRM {SPAKE2 message, mac}
phone → PC   PAIR_VERIFY  {mac}
             ── both derive and persist a long-term key ──
PC   → phone READY       {stream_config}
phone → PC   STREAM_START
```

Pairing uses SPAKE2 over the 6-digit code, so the short code never crosses the wire
and cannot be brute-forced offline. The long-term key is stored in the iOS Keychain
and via DPAPI on Windows. Reconnects skip straight from `SERVER_INFO` to `READY`.

An unpaired device may complete `HELLO` and nothing else. It cannot stream, cannot
read settings, and cannot enumerate anything about the PC beyond what the Bonjour
TXT record already broadcasts.

### What encryption does and does not cover

Per project decision, **media encryption is a toggle, default off** — on a trusted
LAN the CPU and latency are better spent elsewhere.

The control channel is **always** authenticated with an HMAC keyed on the long-term
pairing key, regardless of that toggle. That is not optional: without it, anyone on
the network could push control messages that retarget the camera, change resolution,
or start a recording. Authentication of control and confidentiality of media are
separate properties and only the second one is a user preference.

When the media toggle is on, channels 1–3 are encrypted with ChaCha20-Poly1305 under
a key derived from the pairing key, nonce = channel ‖ sequence.

## Control messages

CBOR maps with a `t` (type) key. Unknown keys are ignored; unknown message types are
ignored with a warning. That rule is what lets a newer phone talk to an older PC.

**Phone → PC**

| `t` | Payload | Notes |
|---|---|---|
| `hello` | see handshake | |
| `caps` | camera list, supported resolutions/fps, lens list | sent once after `ready` |
| `orientation` | `{deg, locked}` | drives auto-rotate on the PC |
| `camera_state` | current ISO, exposure, WB, focus, zoom, torch | echoed after every change |
| `thermal` | `{state}` | `nominal`/`fair`/`serious`/`critical` |
| `battery` | `{level, charging}` | |
| `error` | `{code, message}` | |

**PC → phone**

| `t` | Payload | Notes |
|---|---|---|
| `server_info` | see handshake | |
| `ready` | `{codec, width, height, fps, bitrate}` | |
| `set_camera` | `{lens, position}` | front/back, ultra-wide/wide/tele |
| `set_format` | `{codec, width, height, fps, bitrate}` | |
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
  not sent once — a receiver that joins or resets mid-stream must be able to decode
  from the next keyframe with no side channel.
- One access unit per message. No fragmentation at this layer; TCP handles it.
- Keyframe every ~2 s, plus on demand via `request_keyframe`.

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

`v` is a single integer. A receiver that sees a higher `v` than it knows refuses the
connection with a clear message rather than guessing. Additive changes — new control
message types, new keys, new capability flags — do not bump it; the ignore-unknown
rules above cover them.
