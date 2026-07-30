// Bonjour (DNS-SD) advertisement of `_remotecam._tcp`.
//
// Bonjour, never UDP broadcast or multicast: since iOS 14 sending to a broadcast or
// multicast address needs Apple's `com.apple.developer.networking.multicast`
// entitlement, granted by review. Browsing Bonjour is exempt. That is a locked project
// decision, not a preference (root CLAUDE.md).
//
// Uses `DnsServiceRegister` from dnsapi.h, which is part of Windows -- no Bonjour
// redistributable, nothing for the installer to carry.
//
// ONLY ADVERTISE A PORT SOMETHING IS LISTENING ON. A phone that finds the PC and then
// times out connecting looks like a broken app; not appearing at all looks like a
// network problem the user can reason about. TcpListener::start() must have succeeded
// before this is called.
//
// This is the synchronous form, for console tools. windows/app has its own Qt-signal
// variant because a UI needs the state transitions.

#ifndef RCNET_BONJOUR_SERVICE_H
#define RCNET_BONJOUR_SERVICE_H

#include <windows.h>
#include <windns.h>

#include <cstdint>
#include <string>

namespace rcnet {

class BonjourService {
 public:
  BonjourService() = default;
  ~BonjourService();

  BonjourService(const BonjourService&) = delete;
  BonjourService& operator=(const BonjourService&) = delete;

  // Blocks until Windows confirms the registration or `timeoutMillis` elapses. The TXT
  // record carries the four keys protocol.md specifies: v, name, id, caps.
  //
  // `serviceId` must be 16 lowercase hex characters and stable for this PC -- the phone
  // uses it to recognise the same machine across a rename or an IP change.
  HRESULT start(uint16_t port, const std::wstring& displayName, const std::wstring& serviceId,
                const std::wstring& capabilities = L"h264,hevc",
                DWORD timeoutMillis = 10000);
  void stop();

  bool advertising() const { return advertising_; }

 private:
  struct Registration;

  Registration* registration_ = nullptr;
  bool advertising_ = false;
};

// A stable 16-lowercase-hex identity for this PC, derived from the machine so it
// survives restarts without needing anywhere to persist it.
std::wstring machineServiceId();

// The NetBIOS computer name, or a usable fallback.
std::wstring computerDisplayName();

}  // namespace rcnet

#endif  // RCNET_BONJOUR_SERVICE_H
