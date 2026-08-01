// Tests for the TCP receiver, over real loopback sockets.
//
// Every listener binds port 0 and reads the assigned port back, so the suite never
// collides with a running RemoteCam or with whatever a CI runner already has open. It
// also binds loopback-only, so running it never opens a listening socket to the network
// or trips a firewall prompt.

#include "rcnet/tcp_listener.h"
#include "rcnet/tcp_client.h"

#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rc/control.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const std::string& what) {
  ++g_checks;
  if (!ok) {
    ++g_failures;
    std::printf("  FAIL: %s\n", what.c_str());
  }
}

// Collects everything a connection did, so assertions run on the test thread rather
// than inside a callback where a failure would be hard to attribute.
class RecordingHandler final : public rcnet::SessionHandler {
 public:
  void onConnected(rcnet::Connection& connection) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++connects_;
    connection_ = &connection;
    signal_.notify_all();
  }

  void onFrame(rcnet::Connection&, const rc::wire::Frame& frame) override {
    std::lock_guard<std::mutex> lock(mutex_);
    frames_.push_back(frame);
    signal_.notify_all();
  }

  void onDisconnected(rcnet::Connection&, HRESULT reason) override {
    std::lock_guard<std::mutex> lock(mutex_);
    ++disconnects_;
    lastReason_ = reason;
    connection_ = nullptr;
    signal_.notify_all();
  }

  bool waitForFrames(size_t count, int millis = 5000) {
    std::unique_lock<std::mutex> lock(mutex_);
    return signal_.wait_for(lock, std::chrono::milliseconds(millis),
                            [&] { return frames_.size() >= count; });
  }

  bool waitForDisconnect(int millis = 5000) {
    std::unique_lock<std::mutex> lock(mutex_);
    return signal_.wait_for(lock, std::chrono::milliseconds(millis),
                            [&] { return disconnects_ > 0; });
  }

  bool waitForConnect(int millis = 5000) {
    std::unique_lock<std::mutex> lock(mutex_);
    return signal_.wait_for(lock, std::chrono::milliseconds(millis),
                            [&] { return connects_ > 0; });
  }

  std::vector<rc::wire::Frame> frames() {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_;
  }
  int connects() {
    std::lock_guard<std::mutex> lock(mutex_);
    return connects_;
  }
  int disconnects() {
    std::lock_guard<std::mutex> lock(mutex_);
    return disconnects_;
  }
  HRESULT lastReason() {
    std::lock_guard<std::mutex> lock(mutex_);
    return lastReason_;
  }
  // Sends from the connection the handler is currently holding, which is how the tests
  // exercise the PC -> phone direction.
  HRESULT sendFromServer(const rc::wire::Frame& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (connection_ == nullptr) return E_POINTER;
    return connection_->send(frame);
  }

  HRESULT sendWithoutHandlerSerialization(const rc::wire::Frame& frame) {
    rcnet::Connection* active = nullptr;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      active = connection_;
    }
    if (active == nullptr) return E_POINTER;
    return active->send(frame);
  }

 private:
  std::mutex mutex_;
  std::condition_variable signal_;
  std::vector<rc::wire::Frame> frames_;
  rcnet::Connection* connection_ = nullptr;
  int connects_ = 0;
  int disconnects_ = 0;
  HRESULT lastReason_ = S_OK;
};

// A phone, near enough: a raw socket that writes whatever bytes the test wants,
// including bytes no correct client would ever send.
class FakePhone {
 public:
  bool connect(uint16_t port, int receiveBufferBytes = 0) {
    socket_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_ == INVALID_SOCKET) return false;
    if (receiveBufferBytes > 0 &&
        ::setsockopt(socket_, SOL_SOCKET, SO_RCVBUF,
                     reinterpret_cast<const char*>(&receiveBufferBytes),
                     sizeof(receiveBufferBytes)) != 0) {
      return false;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = ::htons(port);
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    return ::connect(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
  }

  bool sendRaw(const std::vector<uint8_t>& bytes, size_t chunk = 0) {
    const size_t step = chunk == 0 ? bytes.size() : chunk;
    size_t offset = 0;
    while (offset < bytes.size()) {
      const size_t take = (offset + step <= bytes.size()) ? step : bytes.size() - offset;
      const int sent = ::send(socket_, reinterpret_cast<const char*>(bytes.data() + offset),
                              static_cast<int>(take), 0);
      if (sent <= 0) return false;
      offset += static_cast<size_t>(sent);
      if (chunk != 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }

  // Reads until `count` bytes arrive or the peer closes.
  std::vector<uint8_t> receive(size_t count, int millis = 5000) {
    std::vector<uint8_t> out;
    DWORD timeout = static_cast<DWORD>(millis);
    ::setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout),
                 sizeof(timeout));
    while (out.size() < count) {
      uint8_t buffer[4096];
      const int received = ::recv(socket_, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
      if (received <= 0) break;
      out.insert(out.end(), buffer, buffer + received);
    }
    return out;
  }

  void close() {
    if (socket_ != INVALID_SOCKET) {
      ::closesocket(socket_);
      socket_ = INVALID_SOCKET;
    }
  }

  ~FakePhone() { close(); }

 private:
  SOCKET socket_ = INVALID_SOCKET;
};

// ---------------------------------------------------------------------------

void testAcceptsAndReceives() {
  std::printf("Accept and receive\n");

  RecordingHandler handler;
  rcnet::TcpListener listener;
  check(SUCCEEDED(listener.start(0, &handler, true)), "listener starts on an ephemeral port");
  check(listener.listening(), "listener reports listening");
  check(listener.boundPort() != 0, "an ephemeral port was assigned and read back");

  FakePhone phone;
  check(phone.connect(listener.boundPort()), "phone connects");
  check(handler.waitForConnect(), "the handler sees the connection");

  std::vector<uint8_t> stream;
  const rc::control::Message hello =
      rc::control::hello("Test iPhone", "0123456789abcdef", "iPhone", {"h264"});
  const std::vector<uint8_t> helloPayload = hello.encode();
  rc::wire::encode(0, 0, 0, helloPayload.data(), helloPayload.size(), stream);
  check(phone.sendRaw(stream), "phone sends hello");
  check(handler.waitForFrames(1), "the frame arrives");

  const std::vector<rc::wire::Frame> frames = handler.frames();
  check(frames.size() == 1 && frames[0].channel == 0, "it is a control frame");
  if (frames.size() == 1) {
    rc::control::Message parsed;
    rc::cbor::Error cborError = rc::cbor::Error::None;
    check(rc::control::Message::decode(frames[0].payload, parsed, cborError) ==
              rc::control::Error::None,
          "the payload decodes as a control message");
    rc::control::Hello parsedHello;
    check(rc::control::parseHello(parsed, parsedHello) &&
              parsedHello.deviceId == "0123456789abcdef",
          "hello survives the whole path intact");
  }

  phone.close();
  check(handler.waitForDisconnect(), "an orderly peer close is reported");
  check(handler.lastReason() == S_OK, "and reported as orderly, not as a failure");
  listener.stop();
}

void testSplitDelivery() {
  std::printf("Byte-at-a-time delivery\n");

  RecordingHandler handler;
  rcnet::TcpListener listener;
  check(SUCCEEDED(listener.start(0, &handler, true)), "listener starts");

  FakePhone phone;
  check(phone.connect(listener.boundPort()), "phone connects");
  check(handler.waitForConnect(), "connection established");

  // A large payload sent one byte at a time straddles the header across many reads,
  // which is the case a decoder tested only with whole frames gets wrong in production.
  std::vector<uint8_t> stream;
  const std::vector<uint8_t> payload(600, 0x5a);
  rc::wire::encode(1, rc::wire::flags::kKeyframe, 42, payload.data(), payload.size(), stream);
  check(phone.sendRaw(stream, 1), "phone dribbles the frame one byte at a time");
  check(handler.waitForFrames(1), "the frame still reassembles");

  const std::vector<rc::wire::Frame> frames = handler.frames();
  check(frames.size() == 1, "exactly one frame");
  if (frames.size() == 1) {
    check(frames[0].isKeyframe() && frames[0].ptsMicros == 42, "header fields survive");
    check(frames[0].payload == payload, "the payload is byte-identical");
  }
  listener.stop();
}

void testFramingErrorClosesConnection() {
  std::printf("A framing error closes the connection\n");

  RecordingHandler handler;
  rcnet::TcpListener listener;
  check(SUCCEEDED(listener.start(0, &handler, true)), "listener starts");

  FakePhone phone;
  check(phone.connect(listener.boundPort()), "phone connects");
  check(handler.waitForConnect(), "connection established");

  // Claims 16 MiB + 1 and sends no body. The receiver must hang up on the header alone
  // rather than buffer on behalf of a peer it has already decided to drop.
  const uint32_t advertised = rc::wire::kMaxPayloadBytes + 1;
  const std::vector<uint8_t> header = {
      static_cast<uint8_t>(advertised >> 24), static_cast<uint8_t>(advertised >> 16),
      static_cast<uint8_t>(advertised >> 8),  static_cast<uint8_t>(advertised),
      1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  check(phone.sendRaw(header), "phone sends an oversized header");
  check(handler.waitForDisconnect(), "the receiver hangs up");
  check(handler.lastReason() != S_OK, "and reports it as a failure, not an orderly close");
  check(handler.frames().empty(), "no frame was produced");
  listener.stop();
}

void testReservedBitsRejected() {
  std::printf("Reserved header bits close the connection\n");

  RecordingHandler handler;
  rcnet::TcpListener listener;
  check(SUCCEEDED(listener.start(0, &handler, true)), "listener starts");

  FakePhone phone;
  check(phone.connect(listener.boundPort()), "phone connects");
  check(handler.waitForConnect(), "connection established");

  // Reserved u16 non-zero. Accepting it would make those bytes unusable for a future
  // version, because peers would already be setting them.
  const std::vector<uint8_t> header = {0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0};
  check(phone.sendRaw(header), "phone sends a non-zero reserved field");
  check(handler.waitForDisconnect(), "the receiver hangs up");
  check(handler.lastReason() != S_OK, "reported as a failure");
  listener.stop();
}

void testServerToPhoneSend() {
  std::printf("PC -> phone send\n");

  RecordingHandler handler;
  rcnet::TcpListener listener;
  check(SUCCEEDED(listener.start(0, &handler, true)), "listener starts");

  FakePhone phone;
  check(phone.connect(listener.boundPort()), "phone connects");
  check(handler.waitForConnect(), "connection established");

  rc::wire::Frame ready;
  ready.channel = 0;
  ready.payload = rc::control::ready(rc::control::conservativeDefault()).encode();
  check(SUCCEEDED(handler.sendFromServer(ready)), "the server sends ready");

  const std::vector<uint8_t> received = phone.receive(rc::wire::kHeaderBytes + ready.payload.size());
  check(received.size() == rc::wire::kHeaderBytes + ready.payload.size(),
        "the phone receives the whole frame");

  rc::wire::Decoder decoder;
  std::vector<rc::wire::Frame> frames;
  check(decoder.append(received, frames) == rc::wire::Error::None, "it is well framed");
  check(frames.size() == 1, "one frame");
  if (frames.size() == 1) {
    rc::control::Message parsed;
    rc::cbor::Error cborError = rc::cbor::Error::None;
    check(rc::control::Message::decode(frames[0].payload, parsed, cborError) ==
              rc::control::Error::None,
          "and a valid control message");
    check(parsed.type == "ready", "it is ready");
  }
  listener.stop();
}

void testLargeFrameSurvivesPartialSends() {
  std::printf("A large frame survives partial sends\n");

  RecordingHandler handler;
  rcnet::TcpListener listener;
  check(SUCCEEDED(listener.start(0, &handler, true)), "listener starts");

  FakePhone phone;
  check(phone.connect(listener.boundPort()), "phone connects");
  check(handler.waitForConnect(), "connection established");

  // Comfortably past any socket buffer, so send() returns short and the loop that
  // handles it is actually exercised. A 1080p keyframe is this order of magnitude.
  rc::wire::Frame big;
  big.channel = 1;
  big.flags = rc::wire::flags::kKeyframe;
  big.payload.resize(2 * 1024 * 1024);
  for (size_t i = 0; i < big.payload.size(); ++i) {
    big.payload[i] = static_cast<uint8_t>(i * 31u);
  }

  std::thread reader([&] {
    const std::vector<uint8_t> received =
        phone.receive(rc::wire::kHeaderBytes + big.payload.size(), 20000);
    rc::wire::Decoder decoder;
    std::vector<rc::wire::Frame> frames;
    check(decoder.append(received, frames) == rc::wire::Error::None, "large frame is well framed");
    check(frames.size() == 1, "the large frame arrives whole");
    if (frames.size() == 1) {
      check(frames[0].payload == big.payload, "every byte survives a partial send");
    }
  });

  check(SUCCEEDED(handler.sendFromServer(big)), "the server sends a 2 MiB frame");
  reader.join();
  listener.stop();
}

void testSecondPhoneRefused() {
  std::printf("One phone at a time\n");

  RecordingHandler handler;
  rcnet::TcpListener listener;
  check(SUCCEEDED(listener.start(0, &handler, true)), "listener starts");

  FakePhone first;
  check(first.connect(listener.boundPort()), "first phone connects");
  check(handler.waitForConnect(), "first connection established");

  // The second connection is accepted by the OS backlog and then closed by us. What
  // matters is that it ends quickly rather than hanging: the phone gets a definite
  // answer and can show it.
  FakePhone second;
  check(second.connect(listener.boundPort()), "second phone's TCP connect succeeds");
  const std::vector<uint8_t> nothing = second.receive(1, 3000);
  check(nothing.empty(), "the second phone is closed rather than served");
  check(handler.connects() == 1, "only one session was ever started");

  listener.stop();
}

void testStopIsClean() {
  std::printf("Stop is clean and idempotent\n");

  RecordingHandler handler;
  rcnet::TcpListener listener;
  check(SUCCEEDED(listener.start(0, &handler, true)), "listener starts");

  FakePhone phone;
  check(phone.connect(listener.boundPort()), "phone connects");
  check(handler.waitForConnect(), "connection established");

  // The connection is deliberately still open. stop() has to unblock an accept() AND a
  // recv(), both parked in the kernel; closing only the listening socket leaves the
  // connection thread blocked forever and stop() never returns. Hanging here is the
  // failure this asserts against, and it is the bug this test found.
  listener.stop();
  check(!listener.listening(), "the listener reports stopped");
  check(handler.waitForDisconnect(), "the live connection was torn down, not abandoned");
  listener.stop();
  check(true, "a second stop does not hang or crash");

  // A listener can be started again afterwards. Deliberately on a fresh ephemeral port
  // rather than the old one: rebinding the same port would be testing Windows TIME_WAIT
  // semantics, not this code.
  rcnet::TcpListener restarted;
  RecordingHandler second;
  check(SUCCEEDED(restarted.start(0, &second, true)), "a listener can be started again");
  restarted.stop();
}

void testStartRejections() {
  std::printf("Start rejections\n");

  RecordingHandler handler;
  rcnet::TcpListener listener;
  check(listener.start(0, nullptr, true) == E_POINTER, "a null handler is refused");
  check(SUCCEEDED(listener.start(0, &handler, true)), "listener starts");
  check(listener.start(0, &handler, true) == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS),
        "starting twice is refused");

  // A port already bound by someone else must fail rather than silently share it.
  rcnet::TcpListener collision;
  RecordingHandler other;
  check(FAILED(collision.start(listener.boundPort(), &other, true)),
        "binding an occupied port fails instead of stealing it");

  // On Windows, SO_REUSEADDR can otherwise bind over an existing listener. The
  // listener's SO_EXCLUSIVEADDRUSE must defeat that stronger hostile case too.
  SOCKET thief = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  check(thief != INVALID_SOCKET, "a raw collision socket opens");
  if (thief != INVALID_SOCKET) {
    BOOL reuse = TRUE;
    check(::setsockopt(thief, SOL_SOCKET, SO_REUSEADDR,
                       reinterpret_cast<const char*>(&reuse), sizeof(reuse)) == 0,
          "the collision socket enables Windows address reuse");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = ::htons(listener.boundPort());
    address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
    check(::bind(thief, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR,
          "SO_EXCLUSIVEADDRUSE prevents a reuse socket from stealing the port");
    ::closesocket(thief);
  }

  listener.stop();
}

void testReusableTcpClient() {
  std::printf("Reusable TCP client\n");

  RecordingHandler handler;
  rcnet::TcpListener listener;
  check(SUCCEEDED(listener.start(0, &handler, true)), "listener starts");

  rcnet::TcpClient phone;
  check(SUCCEEDED(phone.connect("127.0.0.1", listener.boundPort())),
        "client resolves and connects");
  check(phone.valid(), "client reports a live socket");
  check(handler.waitForConnect(), "listener observes reusable client");

  rc::wire::Frame hello;
  hello.channel = static_cast<uint8_t>(rc::wire::Channel::Control);
  hello.payload = rc::control::hello("Emulated iPhone", "0123456789abcdef", "iPhone",
                                     {"h264", "hevc"}).encode();
  check(SUCCEEDED(phone.send(hello)), "client sends a framed hello");
  check(handler.waitForFrames(1), "listener receives the client frame");

  rc::wire::Frame response;
  response.channel = static_cast<uint8_t>(rc::wire::Channel::Control);
  response.payload =
      rc::control::serverInfo("Test PC", "fedcba9876543210", true, false, {"h264"}).encode();
  check(SUCCEEDED(handler.sendFromServer(response)), "listener answers the client");
  rc::wire::Frame received;
  check(SUCCEEDED(phone.receive(received, 5000)), "client reassembles the response");
  rc::control::Message message;
  rc::cbor::Error cborError = rc::cbor::Error::None;
  check(rc::control::Message::decode(received.payload, message, cborError) ==
            rc::control::Error::None &&
            message.type == "server_info",
        "client response carries the expected control message");
  check(phone.receive(received, 25) == HRESULT_FROM_WIN32(ERROR_TIMEOUT),
        "client reports an idle receive timeout distinctly");

  phone.close();
  check(!phone.valid(), "client closes idempotently");
  phone.close();
  listener.stop();
}

void testConcurrentServerSendsStayFramed() {
  std::printf("Concurrent server sends stay framed\n");
  RecordingHandler handler;
  rcnet::TcpListener listener;
  check(SUCCEEDED(listener.start(0, &handler, true)), "listener starts");
  rcnet::TcpClient phone;
  check(SUCCEEDED(phone.connect("127.0.0.1", listener.boundPort())), "client connects");
  check(handler.waitForConnect(), "handler sees client");

  constexpr int kThreads = 4;
  constexpr int kFramesPerThread = 40;
  std::atomic<int> sendFailures{0};
  std::atomic<int> received{0};
  std::atomic<bool> receiveFailed{false};
  std::thread reader([&] {
    for (int index = 0; index < kThreads * kFramesPerThread; ++index) {
      rc::wire::Frame frame;
      if (FAILED(phone.receive(frame, 10000)) || frame.payload.size() != 4096) {
        receiveFailed.store(true);
        return;
      }
      ++received;
    }
  });

  std::vector<std::thread> senders;
  for (int threadIndex = 0; threadIndex < kThreads; ++threadIndex) {
    senders.emplace_back([&, threadIndex] {
      for (int frameIndex = 0; frameIndex < kFramesPerThread; ++frameIndex) {
        rc::wire::Frame frame;
        frame.channel = static_cast<uint8_t>(rc::wire::Channel::Stats);
        frame.ptsMicros = static_cast<uint64_t>(threadIndex * 1000 + frameIndex);
        frame.payload.assign(4096, static_cast<uint8_t>(threadIndex + 1));
        if (FAILED(handler.sendWithoutHandlerSerialization(frame))) ++sendFailures;
      }
    });
  }
  for (std::thread& sender : senders) sender.join();
  reader.join();
  check(sendFailures.load() == 0, "all concurrent sends complete");
  check(!receiveFailed.load() && received.load() == kThreads * kFramesPerThread,
        "headers and payloads never interleave across senders");
  phone.close();
  listener.stop();
}

void testStopInterruptsBlockedSend() {
  std::printf("Stop interrupts a blocked send\n");
  RecordingHandler handler;
  rcnet::TcpListener listener;
  check(SUCCEEDED(listener.start(0, &handler, true)), "listener starts");
  FakePhone phone;
  check(phone.connect(listener.boundPort(), 1024), "non-reading phone connects");
  check(handler.waitForConnect(), "handler sees the non-reading phone");

  rc::wire::Frame frame;
  frame.channel = static_cast<uint8_t>(rc::wire::Channel::Video);
  frame.payload.assign(2 * 1024 * 1024, 0x5a);
  HRESULT sendHr = S_OK;
  std::thread sender([&] {
    // Loop because Windows loopback can absorb a surprising amount of data before a
    // non-reading peer's advertised window finally closes.
    while (SUCCEEDED(sendHr)) sendHr = handler.sendWithoutHandlerSerialization(frame);
  });

  // Give send() enough time to fill the socket window. stop() must shutdown the
  // socket underneath it and complete well inside the configured send timeout bound.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto started = std::chrono::steady_clock::now();
  listener.stop();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  sender.join();

  check(elapsed < std::chrono::seconds(5),
        "listener shutdown remains bounded when the peer stops reading");
  check(FAILED(sendHr), "the interrupted sender receives a failure");
}

}  // namespace

int main() {
  // Unbuffered: this suite drives real sockets and threads, so a regression here can
  // hang rather than fail. Buffered output would be lost on the kill and the log would
  // say nothing about which test was stuck.
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  testAcceptsAndReceives();
  testSplitDelivery();
  testFramingErrorClosesConnection();
  testReservedBitsRejected();
  testServerToPhoneSend();
  testLargeFrameSurvivesPartialSends();
  testSecondPhoneRefused();
  testStopIsClean();
  testStartRejections();
  testReusableTcpClient();
  testConcurrentServerSendsStayFramed();
  testStopInterruptsBlockedSend();

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
