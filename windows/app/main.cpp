#include <windows.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QUrl>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "bonjour_advertiser.h"
#include "frame_producer.h"
#include "rc/control.h"
#include "rcnet/tcp_listener.h"
#include "rcwin/hr.h"

namespace {

constexpr wchar_t kProducerMutexName[] = L"Local\\RemoteCam.QtProducer.Single";

HRESULT sendControl(rcnet::Connection& connection, const rc::control::Message& message,
                    uint8_t channel = static_cast<uint8_t>(rc::wire::Channel::Control)) {
  const std::vector<uint8_t> payload = message.encode();
  return connection.send(channel, 0, 0, payload.data(), payload.size());
}

// The production app implements the protocol only up to the deliberate security
// boundary: it accepts and parses `hello`, answers with `server_info {paired:false}`,
// and never sends `ready`. The insecure path belongs exclusively to rc-fakepc, so a
// Release build cannot accidentally start an unauthenticated camera session.
class AppSessionHandler final : public rcnet::SessionHandler {
 public:
  AppSessionHandler(std::string computerName, std::string serviceId)
      : computerName_(std::move(computerName)), serviceId_(std::move(serviceId)) {}

  void onConnected(rcnet::Connection& connection) override {
    RC_LOG(L"network peer connected from %hs; waiting for hello", connection.peer().c_str());
  }

  void onFrame(rcnet::Connection& connection, const rc::wire::Frame& frame) override {
    if (frame.channel == static_cast<uint8_t>(rc::wire::Channel::Audio)) return;
    if (frame.channel != static_cast<uint8_t>(rc::wire::Channel::Control) &&
        frame.channel != static_cast<uint8_t>(rc::wire::Channel::Stats)) {
      RC_WARN(L"ignoring unauthenticated channel %u from %hs", frame.channel,
              connection.peer().c_str());
      return;
    }

    rc::control::Message message;
    rc::cbor::Error cborError = rc::cbor::Error::None;
    const rc::control::Error error =
        rc::control::Message::decode(frame.payload, message, cborError);
    if (error != rc::control::Error::None) {
      RC_WARN(L"dropping malformed control message from %hs: %hs (%hs)",
              connection.peer().c_str(), rc::control::errorText(error),
              rc::cbor::errorText(cborError));
      return;
    }

    if (message.type != "hello") {
      RC_WARN(L"ignoring unauthenticated control type %hs from %hs", message.type.c_str(),
              connection.peer().c_str());
      return;
    }

    rc::control::Hello hello;
    if (!rc::control::parseHello(message, hello)) {
      RC_WARN(L"dropping hello with missing required fields from %hs",
              connection.peer().c_str());
      return;
    }
    if (hello.version > rc::control::kProtocolVersion) {
      RC_WARN(L"peer %hs speaks protocol %llu, newer than supported version %llu; closing",
              connection.peer().c_str(), static_cast<unsigned long long>(hello.version),
              static_cast<unsigned long long>(rc::control::kProtocolVersion));
      connection.close();
      return;
    }

    RC_LOG(L"hello from %hs (%hs, %hs), id %hs, protocol %llu", hello.deviceName.c_str(),
           hello.platform.c_str(), hello.model.c_str(), hello.deviceId.c_str(),
           static_cast<unsigned long long>(hello.version));
    const rc::control::Message info =
        rc::control::serverInfo(computerName_, serviceId_, false, {"h264", "hevc"});
    const HRESULT sendHr = sendControl(connection, info);
    if (FAILED(sendHr)) {
      RC_WARN(L"server_info send failed: %s", rcwin::hrMessage(sendHr).c_str());
    } else {
      RC_LOG(L"pairing is not specified; reported paired=false and withheld ready");
    }
  }

  void onDisconnected(rcnet::Connection& connection, HRESULT reason) override {
    RC_LOG(L"network peer %hs disconnected: %s", connection.peer().c_str(),
           rcwin::hrMessage(reason).c_str());
  }

 private:
  std::string computerName_;
  std::string serviceId_;
};

}  // namespace

int main(int argc, char* argv[]) {
  QGuiApplication app(argc, argv);
  QGuiApplication::setApplicationName(QStringLiteral("RemoteCam"));
  QGuiApplication::setOrganizationName(QStringLiteral("RemoteCam"));
  rcwin::logInit(L"rc-app");

  // FrameRing has a single-writer contract. Local\ is deliberate: this
  // coordinates ordinary app instances in the current user's session without
  // requiring the SeCreateGlobalPrivilege that an unelevated desktop app does
  // not possess.
  HANDLE producerMutex = ::CreateMutexW(nullptr, TRUE, kProducerMutexName);
  const DWORD mutexError = ::GetLastError();
  const bool producerConflict = producerMutex && mutexError == ERROR_ALREADY_EXISTS;

  rcapp::FrameProducer producer;
  rcapp::BonjourAdvertiser discovery;
  AppSessionHandler sessionHandler(discovery.computerName().toStdString(),
                                   discovery.serviceID().toStdString());
  rcnet::TcpListener listener;
  if (!producerMutex) {
    const HRESULT hr = rcwin::hrFromLastError();
    RC_ERR(L"CreateMutexW(%s) failed: %s", kProducerMutexName, rcwin::hrMessage(hr).c_str());
    producer.setStartupFailure(QStringLiteral("Could not create the per-session producer guard: %1")
                                   .arg(QString::fromStdWString(rcwin::hrMessage(hr))));
  } else if (producerConflict) {
    RC_WARN(L"producer conflict: another Qt producer is already running");
    ::CloseHandle(producerMutex);
    producerMutex = nullptr;
    producer.setProducerConflict();
  } else {
    RC_LOG(L"acquired per-session producer guard %s", kProducerMutexName);
    producer.start();
  }
  // Bind first, advertise second. Reversing this creates a false-success discovery
  // result where the phone can see the PC but every connection times out.
  const HRESULT listenerHr = listener.start(rcnet::kDefaultPort, &sessionHandler);
  if (SUCCEEDED(listenerHr)) {
    discovery.start();
  } else {
    RC_ERR(L"TCP listener could not start: %s", rcwin::hrMessage(listenerHr).c_str());
    discovery.setReceiverFailure(QString::fromStdWString(rcwin::hrMessage(listenerHr)));
  }

  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty(QStringLiteral("frameProducer"), &producer);
  engine.rootContext()->setContextProperty(QStringLiteral("lanDiscovery"), &discovery);
  const QUrl mainUrl(QStringLiteral("qrc:/RemoteCam/qml/Main.qml"));
  QObject::connect(
      &engine, &QQmlApplicationEngine::objectCreated, &app,
      [mainUrl](QObject* object, const QUrl& objectUrl) {
        if (!object && objectUrl == mainUrl) QCoreApplication::exit(EXIT_FAILURE);
      },
      Qt::QueuedConnection);
  engine.load(mainUrl);

  const int result = app.exec();
  listener.stop();
  producer.stop();
  if (producerMutex) {
    ::ReleaseMutex(producerMutex);
    ::CloseHandle(producerMutex);
  }
  return result;
}
