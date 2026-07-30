#include "rcnet/tcp_client.h"

#include <ws2tcpip.h>

#include <climits>
#include <utility>

#include "rcwin/hr.h"

namespace rcnet {
namespace {

HRESULT wsaFailure() {
  const int error = ::WSAGetLastError();
  return error == 0 ? E_FAIL : HRESULT_FROM_WIN32(static_cast<DWORD>(error));
}

}  // namespace

TcpClient::~TcpClient() { close(); }

HRESULT TcpClient::connect(const std::string& host, uint16_t port) {
  if (FAILED(winsock_.status())) return winsock_.status();
  if (host.empty() || port == 0) return E_INVALIDARG;

  close();
  decoder_.reset();
  pending_.clear();

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  const std::string service = std::to_string(port);
  addrinfo* addresses = nullptr;
  const int lookup = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
  if (lookup != 0) return HRESULT_FROM_WIN32(static_cast<DWORD>(lookup));

  HRESULT last = HRESULT_FROM_WIN32(ERROR_HOST_UNREACHABLE);
  for (addrinfo* address = addresses; address != nullptr; address = address->ai_next) {
    SOCKET candidate = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (candidate == INVALID_SOCKET) {
      last = wsaFailure();
      continue;
    }

    BOOL noDelay = TRUE;
    if (::setsockopt(candidate, IPPROTO_TCP, TCP_NODELAY,
                     reinterpret_cast<const char*>(&noDelay), sizeof(noDelay)) == SOCKET_ERROR) {
      last = wsaFailure();
      ::closesocket(candidate);
      continue;
    }

    if (::connect(candidate, address->ai_addr, static_cast<int>(address->ai_addrlen)) == 0) {
      std::lock_guard<std::mutex> lock(socketMutex_);
      socket_ = candidate;
      peer_ = host + ":" + service;
      last = S_OK;
      break;
    }
    last = wsaFailure();
    ::closesocket(candidate);
  }
  ::freeaddrinfo(addresses);
  return last;
}

void TcpClient::close() {
  std::lock_guard<std::mutex> lock(socketMutex_);
  if (socket_ != INVALID_SOCKET) {
    ::shutdown(socket_, SD_BOTH);
    ::closesocket(socket_);
    socket_ = INVALID_SOCKET;
  }
}

bool TcpClient::valid() const {
  std::lock_guard<std::mutex> lock(socketMutex_);
  return socket_ != INVALID_SOCKET;
}

HRESULT TcpClient::send(uint8_t channel, uint8_t frameFlags, uint64_t ptsMicros,
                        const uint8_t* payload, size_t payloadSize) {
  std::vector<uint8_t> bytes;
  if (rc::wire::encode(channel, frameFlags, ptsMicros, payload, payloadSize, bytes) !=
      rc::wire::Error::None) {
    return E_INVALIDARG;
  }
  return sendRaw(bytes);
}

HRESULT TcpClient::send(const rc::wire::Frame& frame) {
  return send(frame.channel, frame.flags, frame.ptsMicros, frame.payload.data(),
              frame.payload.size());
}

HRESULT TcpClient::sendRaw(const uint8_t* bytes, size_t size) {
  if (bytes == nullptr && size != 0) return E_POINTER;
  std::lock_guard<std::mutex> lock(socketMutex_);
  return sendBytesLocked(bytes, size);
}

HRESULT TcpClient::sendRaw(const std::vector<uint8_t>& bytes) {
  return sendRaw(bytes.data(), bytes.size());
}

HRESULT TcpClient::sendBytesLocked(const uint8_t* bytes, size_t size) {
  if (socket_ == INVALID_SOCKET) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);
  size_t sent = 0;
  while (sent < size) {
    const size_t remaining = size - sent;
    const int chunk = static_cast<int>(remaining > static_cast<size_t>(INT_MAX)
                                           ? static_cast<size_t>(INT_MAX)
                                           : remaining);
    const int result =
        ::send(socket_, reinterpret_cast<const char*>(bytes + sent), chunk, 0);
    if (result == SOCKET_ERROR) return wsaFailure();
    if (result == 0) return HRESULT_FROM_WIN32(ERROR_CONNECTION_ABORTED);
    sent += static_cast<size_t>(result);
  }
  return S_OK;
}

HRESULT TcpClient::receive(rc::wire::Frame& out, DWORD timeoutMillis) {
  if (!pending_.empty()) {
    out = std::move(pending_.front());
    pending_.pop_front();
    return S_OK;
  }

  // recv is intentionally not held under socketMutex_: close() must be able to abort a
  // blocking receive from another thread. Taking a snapshot is safe because Windows
  // does not recycle a just-closed socket handle until closesocket returns.
  SOCKET active = INVALID_SOCKET;
  {
    std::lock_guard<std::mutex> lock(socketMutex_);
    active = socket_;
  }
  if (active == INVALID_SOCKET) return HRESULT_FROM_WIN32(ERROR_INVALID_STATE);

  const DWORD timeout = timeoutMillis;
  if (::setsockopt(active, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                   sizeof(timeout)) == SOCKET_ERROR) {
    return wsaFailure();
  }

  std::vector<uint8_t> chunk(64 * 1024);
  while (pending_.empty()) {
    const int received =
        ::recv(active, reinterpret_cast<char*>(chunk.data()), static_cast<int>(chunk.size()), 0);
    if (received == 0) return HRESULT_FROM_WIN32(ERROR_CONNECTION_ABORTED);
    if (received == SOCKET_ERROR) {
      const int error = ::WSAGetLastError();
      if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) {
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
      }
      return HRESULT_FROM_WIN32(static_cast<DWORD>(error));
    }

    std::vector<rc::wire::Frame> frames;
    const rc::wire::Error error =
        decoder_.append(chunk.data(), static_cast<size_t>(received), frames);
    for (rc::wire::Frame& frame : frames) pending_.push_back(std::move(frame));
    if (error != rc::wire::Error::None) return E_INVALIDARG;
  }

  out = std::move(pending_.front());
  pending_.pop_front();
  return S_OK;
}

}  // namespace rcnet
