// Stable identifiers shared by every RemoteCam Windows component.
//
// The CLSID is written into the registry at install time and handed to
// MFCreateVirtualCamera as its `sourceId`, so it must never change once shipped.
// Changing it orphans the existing registration and leaves behind a camera that
// enumerates but can never be instantiated -- the exact failure this file exists to
// keep everyone from causing independently.

#ifndef RCWIN_GUIDS_H
#define RCWIN_GUIDS_H

#include <guiddef.h>

namespace rcwin {

// {F6328D5A-CF0B-4837-AC0B-5F1E54CD2F25}
inline constexpr GUID kSourceClsid = {
    0xf6328d5a, 0xcf0b, 0x4837, {0xac, 0x0b, 0x5f, 0x1e, 0x54, 0xcd, 0x2f, 0x25}};

// The same value in the string form the registry and MFCreateVirtualCamera want.
// Kept as a literal rather than formatted at runtime so a mismatch is a compile-time
// diff rather than a silent registration against the wrong key.
inline constexpr wchar_t kSourceClsidString[] = L"{F6328D5A-CF0B-4837-AC0B-5F1E54CD2F25}";

// Shown in every camera picker, and how rc-vcam-probe locates the device.
inline constexpr wchar_t kFriendlyName[] = L"RemoteCam";
inline constexpr wchar_t kComDescription[] = L"RemoteCam Virtual Camera Source";

// Kernel object names for the frame handoff.
//
// Global\ is mandatory, not stylistic: rc-vcam.dll is loaded by the Frame Server
// (svchost -k Camera, LOCAL SERVICE) in Session 0, while the process producing frames
// runs in the user's interactive session. A Local\ name would resolve to two different
// objects in the two sessions and the handoff would silently never connect.
inline constexpr wchar_t kFrameSectionName[] = L"Global\\RemoteCam.Frames.0";
inline constexpr wchar_t kFrameEventName[] = L"Global\\RemoteCam.Frame.0";

// Registry location of the COM registration.
//
// HKLM, never HKCR. UAC redirects HKCR writes to a per-user hive that Session 0 cannot
// read, and the failure mode is a camera that enumerates but never delivers a frame --
// which looks like a media source bug and costs hours to trace back to the registry.
inline constexpr wchar_t kClsidKeyPath[] =
    L"SOFTWARE\\Classes\\CLSID\\{F6328D5A-CF0B-4837-AC0B-5F1E54CD2F25}";
inline constexpr wchar_t kInprocKeyPath[] =
    L"SOFTWARE\\Classes\\CLSID\\{F6328D5A-CF0B-4837-AC0B-5F1E54CD2F25}\\InprocServer32";

}  // namespace rcwin

#endif  // RCWIN_GUIDS_H
