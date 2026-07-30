#include "rcnet/tcp_listener.h"

#include <ws2tcpip.h>

#include <vector>

#include "rcwin/hr.h"

namespace rcnet {
namespace {

// Big enough that a 1080p keyframe is a handful of reads rather than hundreds, small
// enough to sit on the stack of a connection thread.
constexpr int kReceiveChunkBytes = 64 * 1024;

HRESULT hrFromWsa(int error) {
  return error == 0 ? S_OK : HRESULT_FROM_WIN32(static_cast<DWORD>(error));
}

HRESULT lastWsaError() { return hrFromWsa(::WSAGetLastError()); }

std::string describePeer(SOCKET socket) {
  sockaddr_storage address{};
  int length = sizeof(address);
  if (::getpeername(socket, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    return "unknown";
  }
  char host[NI_MAXHOST] = {};
  char service[NI_MAXSERV] = {};
  if (::getnameinfo(reinterpret_cast<sockaddr*>(&address), length, host, sizeof(host), service,
                    sizeof(service), NI_NUMERICHOST | NI_NUMERICSERV) != 0) {
    return "unknown";
  }
  return std::string(host) + ":" + service;
}

}  // namespace

WinsockScope::WinsockScope() {
  WSADATA data{};
  const int result = ::WSAStartup(MAKEWORD(2, 2), &data);
  if (result != 0) {
    status_ = hrFromWsa(result);
    RC_ERR(L"WSAStartup failed: %s", rcwin::hrMessage(status_).c_str());
  }
}

WinsockScope::~WinsockScope() {
  if (SUCCEEDED(status_)) ::WSACleanup();
}

Connection::Connection(SOCKET socket) : socket_(socket) {
  if (socket_ != INVALID_SOCKET) peer_ = describePeer(socket_);
}

Connection::~Connection() { close(); }

HRESULT Connection::send(uint8_t channel, uint8_t frameFlags, uint64_t ptsMicros,
                         const uint8_t* payload, size_t payloadSize) {
  if (socket_ == INVALID_SOCKET) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);

  std::vector<uint8_t> bytes;
  const rc::wire::Error framingError =
      rc::wire::encode(channel, frameFlags, ptsMicros, payload, payloadSize, bytes);
  if (framingError != rc::wire::Error::None) {
    RC_ERR(L"refusing to send an unframable message: %hs", rc::wire::errorText(framingError));
    return E_INVALIDARG;
  }

  size_t sent = 0;
  while (sent < bytes.size()) {
    // send() is free to accept less than asked, and does once a frame outgrows the
    // socket buffer. Treating its return as all-or-nothing corrupts the stream in a way
    // that only shows up on large keyframes.
    const int chunk = static_cast<int>(
        (bytes.size() - sent) > INT_MAX ? INT_MAX : (bytes.size() - sent));
    const int result = ::send(socket_, reinterpret_cast<const char*>(bytes.data() + sent),
                              chunk, 0);
    if (result == SOCKET_ERROR) {
      const HRESULT hr = lastWsaError();
      RC_WARN(L"send to %hs failed: %s", peer_.c_str(), rcwin::hrMessage(hr).c_str());
      return hr;
    }
    sent += static_cast<size_t>(result);
  }
  return S_OK;
}

HRESULT Connection::send(const rc::wire::Frame& frame) {
  return send(frame.channel, frame.flags, frame.ptsMicros, frame.payload.data(),
              frame.payload.size());
}

void Connection::shutdownSend() {
  if (socket_ != INVALID_SOCKET) ::shutdown(socket_, SD_SEND);
}

void Connection::close() {
  if (socket_ != INVALID_SOCKET) {
    ::closesocket(socket_);
    socket_ = INVALID_SOCKET;
  }
}

TcpListener::~TcpListener() { stop(); }

HRESULT TcpListener::start(uint16_t port, SessionHandler* handler, bool loopbackOnly) {
  if (FAILED(winsock_.status())) return winsock_.status();
  if (handler == nullptr) return E_POINTER;
  if (listenSocket_ != INVALID_SOCKET) return HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);

  stopping_.store(false, std::memory_order_release);

  listenSocket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listenSocket_ == INVALID_SOCKET) {
    const HRESULT hr = lastWsaError();
    RC_ERR(L"socket() failed: %s", rcwin::hrMessage(hr).c_str());
    return hr;
  }

  // Deliberately NOT SO_REUSEADDR. On Windows it permits two sockets to bind the same
  // port outright -- it is not the polite TIME_WAIT reuse it is on BSD -- so setting it
  // would let a second RemoteCam silently steal the phone's connections.
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = ::htons(port);
  address.sin_addr.s_addr = loopbackOnly ? ::htonl(INADDR_LOOPBACK) : ::htonl(INADDR_ANY);

  if (::bind(listenSocket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
      SOCKET_ERROR) {
    const HRESULT hr = lastWsaError();
    RC_ERR(L"bind to port %u failed: %s", port, rcwin::hrMessage(hr).c_str());
    ::closesocket(listenSocket_);
    listenSocket_ = INVALID_SOCKET;
    return hr;
  }

  // Read the port back rather than echoing the argument: with port 0 the kernel picked
  // it, and that is the value a caller needs.
  sockaddr_in bound{};
  int boundLength = sizeof(bound);
  if (::getsockname(listenSocket_, reinterpret_cast<sockaddr*>(&bound), &boundLength) == 0) {
    boundPort_.store(::ntohs(bound.sin_port), std::memory_order_release);
  } else {
    boundPort_.store(port, std::memory_order_release);
  }

  if (::listen(listenSocket_, SOMAXCONN) == SOCKET_ERROR) {
    const HRESULT hr = lastWsaError();
    RC_ERR(L"listen failed: %s", rcwin::hrMessage(hr).c_str());
    ::closesocket(listenSocket_);
    listenSocket_ = INVALID_SOCKET;
    return hr;
  }

  listening_.store(true, std::memory_order_release);
  RC_LOG(L"listening on port %u", boundPort_.load(std::memory_order_acquire));
  acceptThread_ = std::thread([this, handler] { acceptLoop(handler); });
  return S_OK;
}

void TcpListener::stop() {
  stopping_.store(true, std::memory_order_release);

  // Closing the listening socket is what unblocks accept(); there is no cancellable
  // wait to signal instead.
  if (listenSocket_ != INVALID_SOCKET) {
    ::closesocket(listenSocket_);
    listenSocket_ = INVALID_SOCKET;
  }
  listening_.store(false, std::memory_order_release);

  // And close the live connection, which is what unblocks its recv. Without this the
  // connection thread stays parked in the kernel and the join below never returns.
  {
    std::lock_guard<std::mutex> lock(activeMutex_);
    if (activeConnection_ != nullptr) activeConnection_->close();
  }

  if (acceptThread_.joinable()) acceptThread_.join();
  if (connectionThread_.joinable()) connectionThread_.join();
}

void TcpListener::acceptLoop(SessionHandler* handler) {
  while (!stopping_.load(std::memory_order_acquire)) {
    const SOCKET accepted = ::accept(listenSocket_, nullptr, nullptr);
    if (accepted == INVALID_SOCKET) {
      if (stopping_.load(std::memory_order_acquire)) break;
      const HRESULT hr = lastWsaError();
      RC_WARN(L"accept failed: %s", rcwin::hrMessage(hr).c_str());
      break;
    }

    if (connectionActive_.exchange(true, std::memory_order_acq_rel)) {
      // A phone is already connected. Refuse immediately rather than leaving the socket
      // open and unread, which would look like a successful connection that never
      // answers.
      RC_WARN(L"refusing a second phone; one connection at a time in v1");
      ::closesocket(accepted);
      continue;
    }

    // The previous connection's thread has finished (connectionActive_ was false) but
    // may not have been joined yet.
    if (connectionThread_.joinable()) connectionThread_.join();
    connectionThread_ = std::thread([this, accepted, handler] {
      serveConnection(accepted, handler);
      connectionActive_.store(false, std::memory_order_release);
    });
  }
}

void TcpListener::serveConnection(SOCKET socket, SessionHandler* handler) {
  // TCP_NODELAY on the accepted socket, not the listening one. Nagle would coalesce a
  // control reply with the next frame and add up to 40 ms to a 45 ms latency budget.
  BOOL noDelay = TRUE;
  if (::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay),
                   sizeof(noDelay)) == SOCKET_ERROR) {
    RC_WARN(L"could not set TCP_NODELAY: %s", rcwin::hrMessage(lastWsaError()).c_str());
  }

  Connection connection(socket);
  {
    std::lock_guard<std::mutex> lock(activeMutex_);
    activeConnection_ = &connection;
  }
  RC_LOG(L"phone connected from %hs", connection.peer().c_str());
  handler->onConnected(connection);

  rc::wire::Decoder decoder;
  std::vector<uint8_t> chunk(kReceiveChunkBytes);
  std::vector<rc::wire::Frame> frames;
  HRESULT reason = S_OK;

  while (!stopping_.load(std::memory_order_acquire)) {
    const int received =
        ::recv(socket, reinterpret_cast<char*>(chunk.data()), kReceiveChunkBytes, 0);
    if (received == 0) break;  // orderly close
    if (received == SOCKET_ERROR) {
      if (!stopping_.load(std::memory_order_acquire)) {
        reason = lastWsaError();
        RC_WARN(L"recv from %hs failed: %s", connection.peer().c_str(),
                rcwin::hrMessage(reason).c_str());
      }
      break;
    }

    frames.clear();
    const rc::wire::Error framingError =
        decoder.append(chunk.data(), static_cast<size_t>(received), frames);

    // Frames that completed before the error are still delivered: they are the context
    // for whatever went wrong, and the connection is closing either way.
    for (const rc::wire::Frame& frame : frames) handler->onFrame(connection, frame);

    if (framingError != rc::wire::Error::None) {
      RC_ERR(L"framing error from %hs (%hs); closing the connection",
             connection.peer().c_str(), rc::wire::errorText(framingError));
      reason = E_INVALIDARG;
      break;
    }
  }

  handler->onDisconnected(connection, reason);
  RC_LOG(L"phone disconnected (%s)", rcwin::hrMessage(reason).c_str());
  {
    // Cleared before the close, so stop() can never reach a Connection that is about
    // to leave scope.
    std::lock_guard<std::mutex> lock(activeMutex_);
    activeConnection_ = nullptr;
  }
  connection.close();
}

}  // namespace rcnet
