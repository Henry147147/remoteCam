# iOS ↔ Windows backend handoff

Status: **Windows seams implemented; joint security and live integration required**.
The iOS app has capture, VideoToolbox encode, framed TCP transport, discovery UI,
manual camera controls, telemetry, reconnect handling, and Live Activity. Windows
now has the bounded listener, codecs, control parsing, decoder/transform seams, ABR,
Bonjour lifecycle, and an explicit Debug-only insecure development harness whose
session bypass is compiled out of Release. Neither side can
complete a secure production session until the underspecified security parts of the
normative protocol are settled and implemented.

Do not treat this file as a replacement for [`protocol.md`](protocol.md). It records
the implementation-ready message shapes already emitted by iOS and calls out the
decisions that must be made jointly before either side can finish the security layer.

## Backend seam status

1. **Done and tested:** the Windows listener sets `TCP_NODELAY`, uses the shared
   16-byte framing, enforces the 16 MiB and reserved-bit bounds, ignores unknown
   channels, permits one phone, and stops cleanly.
2. **Done and tested:** the Qt app binds TCP 7890 before starting the
   `_remotecam._tcp` advertiser. Its persisted 16-lowercase-hex service ID and the
   `v`, `name`, `id`, and `caps` TXT keys remain the discovery contract.
3. **Done and tested:** portable bounded CBOR matches Swift's RFC 7049-style
   length-first, then bytewise text-key order (not RFC 8949 plain bytewise order),
   strict UTF-8 and binary64 camera values while tolerating unknown keys/types.
4. **Seam complete, live path open:** H.264/HEVC Annex-B parameter-set validation,
   FFmpeg D3D11VA decoder construction, and PTS-preserving pipeline order are tested.
   The production app does not yet feed decoded output into the virtual-camera ring.
5. **Policy seam complete:** the ABR controller implements fast backoff and slow
   recovery, and `rc-fakepc` exercises 2 Hz `stats`/`target_bitrate`. Production stats
   wait behind the authenticated session state machine.
6. **Control seam complete:** `set_format`, camera/control messages, telemetry, and
   caps parse/encode paths are tested and exercised by `rc-fakepc`. Coordinated
   thermal downshift policy still belongs in the integrated production session.

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

**Required protocol follow-up:** the Windows backend now sends
`set_format {..., generation}` and will not commit the new decoder configuration until
iOS replies `format_ack {generation}` after the capture/encoder rebuild and before the
first new-format access unit. If that rebuild fails, iOS sends
`format_reject {generation, code, message}`; Windows retains its committed decoder,
requests a keyframe, and resumes the old generation. The iOS wire receiver must also
reject `v != 1`, non-canonical 16-lowercase-hex identities, and fragment flags to match
the hardened v1 contract in `protocol.md`.

## Security contract and remaining integration

The cryptographic choices are now normative in `protocol.md`: RFC 9382 SPAKE2 on
P-256/SHA-256, phone/client role A and PC/server role B, exact M/N constants and
transcript, scrypt code stretching, two-way key confirmation, nonce-bound reconnect
authentication, direction/channel-separated keys and prefixes, HMAC envelopes, and
ChaCha20-Poly1305 media/statistics envelopes. Treat those byte layouts as fixtures;
do not infer a different layout from the older prose in either platform handbook.

Windows now provides `rcwin-security`: exact OpenSSL 3.5.7 or fail closed, RFC vectors,
DPAPI atomic storage, per-source/global throttling, `PairingServer`,
`StoredSessionSecurity`, and a session protector. `SessionController` has a production
constructor accepting `ISessionSecurity`; a claimed ID only starts a record lookup,
and `ready` is sent inside an authenticated envelope only after the client proof.
The legacy `ITrustPolicy` constructor remains solely for existing fake-phone/E2E test
executables. At this checkpoint the shipping Qt app still constructs
`RejectingTrustPolicy`, so UI pairing orchestration and selecting the secure constructor
remain explicit desktop-app integration work rather than an implied insecure fallback.

iOS must implement the matching role-A PAKE, persist the verified long-term key in the
Keychain, exchange `auth_challenge`/`auth_response`/`auth_confirm`, and install the
protector before accepting `ready`. It must reject non-minimal CBOR integers and extra
fields on known v1 messages. Debug builds may retain `--allow-insecure-session` for the
local harness; Release must never accept an unauthenticated `ready`.

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

The Qt app has a Bonjour advertiser using the Windows inbox `DnsServiceRegister` API,
and the iOS app browses it with `NWBrowser`. Both sides reserve TCP port `7890` for the
current integration build. Manual IP/hostname plus port entry remains available in
iOS for networks that block Bonjour or isolate clients.

The Qt app now binds port `7890`, sets `TCP_NODELAY`, accepts and parses iOS `hello`,
then starts the advertiser. It answers `server_info {paired:false}` and deliberately
withholds `ready`; production handshake work resumes only after the shared security
contract is normative. The installer adds/removes the private-profile inbound
firewall rule for that listener. Before calling discovery verified, run the installed
Qt app on Windows and confirm a physical iPhone on the same LAN lists the PC, removes
it when the app closes, and can open the advertised endpoint. The iOS 27 beta
simulator currently reports stale interface indexes while browsing host-published
mDNS records, so the simulator is not a substitute for that physical-device matrix.
