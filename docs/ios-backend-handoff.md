# iOS ↔ Windows backend handoff

Status: **action required from the Windows/backend agent**. The iOS app now has a
real capture, VideoToolbox encode, framed TCP transport, discovery UI, manual camera
controls, telemetry, reconnect handling, and Live Activity. It cannot complete a
secure production session until the backend and the underspecified security parts of
the normative protocol are implemented.

Do not treat this file as a replacement for [`protocol.md`](protocol.md). It records
the implementation-ready message shapes already emitted by iOS and calls out the
decisions that must be made jointly before either side can finish the security layer.

## Backend work that can start immediately

1. Add the Windows TCP listener with `TCP_NODELAY` and the 16-byte framing from
   `protocol.md`. Close on payloads over 16 MiB, nonzero reserved header bytes, or
   reserved flag bits. Ignore channel 2.
2. **Implemented in `windows/app`:** publish `_remotecam._tcp` with TXT keys `v`,
   `name`, `id`, and `caps`. The `id` is exactly 16 lowercase hex characters and is
   persisted per Windows user so it survives rename, reboot, and IP changes. The
   Windows build and a physical iPhone still need the live LAN verification below.
3. Add a bounded CBOR codec and ignore unknown keys/message types. iOS emits RFC 8949
   deterministic map ordering (shorter encoded text keys first, then lexical byte
   order) and encodes floating-point camera values as binary64.
4. Add H.264/HEVC Annex-B receive and decoder plumbing. Each keyframe emitted by iOS
   includes its parameter sets. Preserve the frame header's monotonic
   `pts_micros` through diagnostics and the virtual-camera sink.
5. Send stats at about 2 Hz, including `target_bitrate`. iOS applies that value to
   `kVTCompressionPropertyKey_AverageBitRate` without rebuilding the encoder.
6. On `thermal {state: "serious"}` or `"critical"`, choose a format from the phone's
   advertised capabilities and send `set_format`; prefer 1280×720 at 30 fps. The
   current protocol has no safe phone-initiated format-change message, so the phone
   reports the condition and the PC owns the coordinated downshift.

## Message shapes emitted by iOS

The initial phone message is:

```text
hello {
  v: 1,
  device_name: text,
  device_id: 16-lowercase-hex,
  platform: "ios",
  model: text,
  caps: ["h264", "hevc"]
}
```

After `ready`, iOS configures the selected camera and sends:

```text
caps {
  codecs: ["hevc", "h264"],
  cameras: [{
    id: text, name: text, position: "front" | "back",
    lens: "ultra-wide" | "wide" | "tele" | "true-depth" | "other",
    formats: [{width: uint, height: uint, fps: 30 | 60}]
  }]
}
```

It then sends the current controls and echoes this after every local or remote
change:

```text
camera_state {
  device_id: text | null, position: text, lens: text,
  zoom: float64,
  focus_mode: "auto" | "locked" | "manual", focus: float64,
  exposure_mode: "auto" | "locked" | "manual",
  iso: float64, exposure: float64, ev: float64,
  wb_mode: "auto" | "locked" | "manual", wb: float64,
  torch: bool
}
```

`exposure` is seconds, `wb` is Kelvin, and `focus` is normalized 0…1.

Telemetry messages are:

```text
orientation {deg: float64, locked: false}
thermal {state: "nominal" | "fair" | "serious" | "critical"}
battery {level: float64, charging: bool}
```

The iOS receiver currently handles:

```text
ready {codec, width, height, fps, bitrate}
set_format {codec, width, height, fps, bitrate}
set_camera {lens, position?}
set_control {
  zoom?, focus_mode?, focus?, exposure_mode?, iso?, exposure?, ev?,
  wb_mode?, wb?, torch?, stabilization?
}
request_keyframe {}
set_preview {enabled: bool}
stats {target_bitrate, ...}
```

After capture and encode are live, iOS sends `stream_start {}` and begins video on
channel 1. A subsequent `set_format` rebuilds capture/encode, forces a keyframe, and
does not send another `stream_start`.

## Security decisions required before production pairing

`protocol.md` says “SPAKE2,” “HMAC-authenticated,” and “nonce = channel ‖ sequence,”
but those descriptions do not uniquely define interoperable cryptography. Please
open a small joint protocol change (do not guess independently) that fixes all of the
following:

- SPAKE2 versus SPAKE2+, group/curve, hash, fixed M/N points, point encoding,
  password-to-scalar method, participant identities, transcript encoding, KDF, key
  confirmation MAC, salt length/encoding, and invalid-point handling.
- Whether the six-digit code includes leading zeros and the required online retry
  limit/cooldown. The PC should display the code; it must never cross the wire.
- The exact derivation and Keychain/DPAPI record format for the long-term pairing
  key, including host/device identifiers and key rotation/unpair behavior.
- The authenticated control envelope: where the sequence number and authentication
  tag live, exactly which header/payload bytes are MACed, tag truncation (if any),
  canonical-CBOR requirements, replay window, and reconnect sequence reset rules.
- The ChaCha20-Poly1305 media KDF, full 96-bit nonce layout, sequence transport, AAD,
  tag placement, rekey limits, and whether stats are encrypted with the same or a
  distinct subkey.

Until these are normative, the iOS app intentionally does not send `PAIR_COMMIT` or
persist a pairing key. It also does not pretend unsigned control messages are secure.
A backend-only fake server may skip pairing for local development, but that bypass
must be a test executable or compile-time development option and must never ship in
the Windows app. Debug builds of iOS accept `--allow-insecure-session` as an Xcode
launch argument for this harness; Release builds compile out the bypass and reject
every unauthenticated `ready` message.

## Two protocol ordering decisions

The current handshake says the PC sends `ready {stream_config}` and the phone sends
its detailed `caps` only afterward. A PC cannot reliably choose 4K60, a lens, or even
1080p60 before it knows what the device supports. Choose one:

- Recommended: after authenticated pairing, PC sends a capability request, phone
  sends `caps`, then PC sends `ready`.
- Minimal v1: PC sends a conservative `ready` (1280×720 H.264 at 30 fps), phone sends
  `caps`, then PC upgrades with `set_format`.

The iOS implementation currently supports the minimal path. It will need a small
change if the recommended ordering is adopted.

USB also needs a fixed device-side TCP port and an explicit connection direction.
The Wi-Fi path says the PC listens and phone connects, while the usbmux section says
Windows opens a tunnel to a port where iOS listens. Define that port and whether the
phone sends `hello` immediately on accepted USB connections. Once decided, iOS can
add the `NWListener` without changing framing or session logic.

## Fast backend development loop

Add a `rc-fakepc` or equivalent test target before the full Qt app. It should:

1. publish Bonjour and accept one phone;
2. parse and dump `hello`, `caps`, telemetry, and `camera_state`;
3. send a conservative `ready` in an explicitly insecure development mode;
4. write channel-1 Annex-B access units to disk and verify every flagged keyframe
   begins with the expected H.264 SPS/PPS or HEVC VPS/SPS/PPS;
5. exercise `set_preview`, `set_camera`, `set_control`, `set_format`, stats bitrate,
   and `request_keyframe`.

That gives both machines a reproducible walking skeleton while the production
pairing contract is settled.

## LAN discovery integration checkpoint

The Qt app now registers the Bonjour service with the Windows inbox
`DnsServiceRegister` API and the iOS app browses it with `NWBrowser`. Both sides use
TCP port `7890` for the current integration build. Manual IP/hostname plus port entry
remains available in iOS for networks that block Bonjour or isolate clients.

The Windows transport/backend work is not replaced by the advertisement: the TCP
receiver still needs to bind port `7890`, set `TCP_NODELAY`, accept the iOS `hello`,
and implement the handshake/framing work above. The installer also needs an inbound
Windows Firewall rule for that listener. Before calling discovery verified, run the
Qt app on Windows and confirm a physical iPhone on the same LAN lists the PC, removes
it when the app closes, and can open the advertised endpoint. The iOS 27 beta
simulator currently reports stale interface indexes while browsing host-published
mDNS records, so the simulator is not a substitute for that physical-device matrix.
