// Blocking TCP client used by Windows-side protocol peers and integration tools.
//
// The production PC listens, while an iPhone connects. rc-fakephone must exercise the
// opposite side of that real socket boundary, including DNS/IPv4/IPv6 resolution,
// TCP_NODELAY, partial sends, receive timeouts and streaming frame reassembly. Keeping
// that behavior here avoids an emulator-only transport that accidentally hides bugs.

#ifndef RCNET_TCP_CLIENT_H
#define RCNET_TCP_CLIENT_H

#include <winsock2.h>
#include <windows.h>

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "rc/wire.h"
#include "rcnet/tcp_listener.h"

namespace rcnet {

class TcpClient {
 public:
  TcpClient() = default;
  ~TcpClient();

  TcpClient(const TcpClient&) = delete;
  TcpClient& operator=(const TcpClient&) = delete;

  // Resolves `host` with AF_UNSPEC and tries every result. A numeric IPv4 or IPv6
  // address and an ordinary hostname therefore share the same code path.
  HRESULT connect(const std::string& host, uint16_t port);
  void close();

  bool valid() const;
  const std::string& peer() const { return peer_; }

  HRESULT send(uint8_t channel, uint8_t frameFlags, uint64_t ptsMicros,
               const uint8_t* payload, size_t payloadSize);
  HRESULT send(const rc::wire::Frame& frame);

  // Sends bytes without framing. Only fault-injection tests should call this.
  HRESULT sendRaw(const uint8_t* bytes, size_t size);
  HRESULT sendRaw(const std::vector<uint8_t>& bytes);

  // Returns S_OK with one complete frame, HRESULT_FROM_WIN32(ERROR_TIMEOUT) when no
  // frame completes before timeout, and ERROR_CONNECTION_ABORTED for an orderly close.
  HRESULT receive(rc::wire::Frame& out, DWORD timeoutMillis = 5000);

 private:
  HRESULT sendBytesLocked(const uint8_t* bytes, size_t size);

  WinsockScope winsock_;
  mutable std::mutex socketMutex_;
  SOCKET socket_ = INVALID_SOCKET;
  std::string peer_;
  rc::wire::Decoder decoder_;
  std::deque<rc::wire::Frame> pending_;
};

}  // namespace rcnet

#endif  // RCNET_TCP_CLIENT_H
