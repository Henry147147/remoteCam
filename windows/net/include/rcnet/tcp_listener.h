// The Windows side of the phone connection.
//
// One TCP connection per device, PC listens and phone connects, TCP_NODELAY on both
// ends (docs/protocol.md "Transport"). Framing and message parsing live in core/ and
// are shared with the iOS client; this file is only the socket, the threads and the
// lifetime.
//
// WHY THE LISTENER OWNS BONJOUR'S START
//
// windows/app/bonjour_advertiser.cpp has always been able to advertise
// `_remotecam._tcp` and has deliberately never been started, because advertising a port
// nothing listens on is worse than not advertising at all: the phone finds the PC,
// connects, and times out, which reads as a broken app rather than a missing feature.
// The advertiser starts only after bind() and listen() have both succeeded.
//
// ONE PHONE AT A TIME
//
// Multi-phone is post-v1 (root CLAUDE.md). A second connection is accepted and closed
// immediately rather than queued, so the phone gets a fast, definite answer instead of
// a socket that hangs. Nothing here forecloses more later -- the connection state is
// already per-Connection, not per-listener.

#ifndef RCNET_TCP_LISTENER_H
#define RCNET_TCP_LISTENER_H

#include <winsock2.h>
// winsock2.h must precede windows.h or winsock.h gets pulled in first and the two
// disagree about every symbol.
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "rc/wire.h"

namespace rcnet {

// The port both sides reserved (docs/ios-backend-handoff.md "Integration checkpoint").
inline constexpr uint16_t kDefaultPort = 7890;

// Winsock refcount. Every socket-owning object holds one; the last one out calls
// WSACleanup. A process-wide init-once would be simpler and would break any host that
// embeds this alongside other Winsock users.
class WinsockScope {
 public:
  WinsockScope();
  ~WinsockScope();
  WinsockScope(const WinsockScope&) = delete;
  WinsockScope& operator=(const WinsockScope&) = delete;

  HRESULT status() const { return status_; }

 private:
  HRESULT status_ = S_OK;
};

class Connection {
 public:
  explicit Connection(SOCKET socket);
  ~Connection();

  Connection(const Connection&) = delete;
  Connection& operator=(const Connection&) = delete;

  // Sends one framed message. Blocking, and loops over partial sends -- a large video
  // frame will not fit in one send() on any socket buffer worth having.
  HRESULT send(uint8_t channel, uint8_t frameFlags, uint64_t ptsMicros,
               const uint8_t* payload, size_t payloadSize);
  HRESULT send(const rc::wire::Frame& frame);

  // Unblocks a reader parked in recv(). Safe from any thread.
  void shutdownSend();
  void close();

  bool valid() const;
  const std::string& peer() const { return peer_; }

 private:
  // One framed message is one indivisible byte stream operation. The listener's
  // session controller, ABR timer and UI can all send concurrently; without this lock
  // their headers and payloads can interleave even though each individual send() is
  // legal. The same lock also makes close-vs-send a defined operation.
  mutable std::mutex socketMutex_;
  SOCKET socket_ = INVALID_SOCKET;
  std::string peer_;
};

// Called on the connection's own thread. A handler that blocks here stops that phone's
// stream and nothing else.
class SessionHandler {
 public:
  virtual ~SessionHandler() = default;
  virtual void onConnected(Connection& connection) = 0;
  virtual void onFrame(Connection& connection, const rc::wire::Frame& frame) = 0;
  // `reason` is S_OK for an orderly peer close, otherwise the failure that ended it --
  // including a framing error, which is fatal by design because the stream position is
  // no longer known.
  virtual void onDisconnected(Connection& connection, HRESULT reason) = 0;
};

class TcpListener {
 public:
  TcpListener() = default;
  ~TcpListener();

  TcpListener(const TcpListener&) = delete;
  TcpListener& operator=(const TcpListener&) = delete;

  // Binds, listens, and starts accepting on a background thread. Pass port 0 to take
  // an ephemeral port and read it back from boundPort(), which is what the tests do so
  // they never collide with a real RemoteCam or with a CI runner's occupied ports.
  //
  // Production is a dual-stack IPv6 listener (IPV6_V6ONLY=0). Binds an IPv4 loopback
  // socket only when `loopbackOnly`, which keeps a test from opening a listening socket
  // to the network and tripping a firewall prompt while remaining reachable at
  // 127.0.0.1 on every Windows CI host.
  HRESULT start(uint16_t port, SessionHandler* handler, bool loopbackOnly = false);
  void stop();

  bool listening() const { return listening_.load(std::memory_order_acquire); }
  uint16_t boundPort() const { return boundPort_.load(std::memory_order_acquire); }

 private:
  void acceptLoop(SessionHandler* handler);
  void serveConnection(SOCKET socket, SessionHandler* handler);

  WinsockScope winsock_;
  SOCKET listenSocket_ = INVALID_SOCKET;
  std::thread acceptThread_;
  std::thread connectionThread_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> listening_{false};
  std::atomic<uint16_t> boundPort_{0};
  // Guards the one-connection-at-a-time rule without needing a mutex on the accept path.
  std::atomic<bool> connectionActive_{false};

  // Closing the listening socket unblocks accept() but does nothing for a connection
  // thread parked in recv(), which would then never exit and hang stop() forever.
  //
  // It has to be closesocket, not shutdown: on Windows shutdown() makes *subsequent*
  // recv calls return 0 but is not documented to unblock one already in progress, and
  // in practice it does not. closesocket does, failing the pending call.
  //
  // The mutex is what keeps that safe. Connection::close() is idempotent and the
  // pointer is cleared under this lock before the owning thread closes, so the socket
  // is closed exactly once and stop() can never touch a Connection that has gone away.
  std::mutex activeMutex_;
  Connection* activeConnection_ = nullptr;
};

}  // namespace rcnet

#endif  // RCNET_TCP_LISTENER_H
