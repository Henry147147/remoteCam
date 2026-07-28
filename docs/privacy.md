# RemoteCam privacy

RemoteCam is designed as a direct connection between your iPhone and your Windows
computer. The project has no RemoteCam cloud service, account system, analytics,
advertising SDK, crash-reporting SDK, or tracking domains.

## Data sent to your computer

While you choose to connect, the iOS app may send the following over the local
network or a future USB tunnel:

- camera video frames;
- camera and lens capabilities and current manual-control values;
- the phone's display name, device model, and a random RemoteCam device identifier;
- device orientation, battery/charging status, and thermal state; and
- connection timing needed for bitrate control and diagnostics.

This data goes to the RemoteCam listener you select. It is not sent to the RemoteCam
developers or to a third-party service. Video recording and screenshots are PC-side
features and are saved only when the user requests them; the Windows app must make
their destination visible.

## Data stored on the phone

The app stores up to ten recent computer entries in its private preferences. A
random device identifier and, once production pairing is implemented, long-term
pairing keys are stored in the iOS Keychain with device-only accessibility. Removing
the app removes its preferences; paired computers should also offer an explicit
unpair action that deletes their Keychain record.

## Permissions

- **Camera:** required only to stream camera video.
- **Local network:** required to discover and connect to a Windows computer using
  Bonjour/TCP.
- **Live Activities:** used to keep a visible system indicator while the camera is
  live. The app does not use ActivityKit push notifications.

RemoteCam does not request microphone, contacts, location, photo library, Bluetooth,
or tracking permission in v1.

## Security status

The production design requires pairing and authenticated control. Optional media
encryption is a user setting. Release builds refuse to stream an unauthenticated
session. Those cryptographic details are still being finalized in the shared
protocol, as tracked in [`ios-backend-handoff.md`](ios-backend-handoff.md); this is
why the repository does not yet describe the app as ready for general use.

Because RemoteCam is open source, this behavior can be inspected in `ios/` and
`windows/`. Distribution-specific App Store privacy disclosures must remain
consistent with this document and with the shipped binaries.
