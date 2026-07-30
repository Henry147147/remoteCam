#include <windows.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QUrl>
#include <cstdlib>
#include <string>

#include "bonjour_advertiser.h"
#include "frame_producer.h"
#include "session_status.h"
#include "rcbackend/session_controller.h"
#include "rcnet/tcp_listener.h"
#include "rcwin/hr.h"

namespace {

constexpr wchar_t kProducerMutexName[] = L"Local\\RemoteCam.QtProducer.Single";

#if defined(RC_APP_E2E_HOST)
// Compiled only into RemoteCam-E2E.exe. The shipping RemoteCam.exe instantiates the
// rejecting policy below and therefore contains no insecure trust implementation.
class InsecureE2ETrustPolicy final : public rcbackend::ITrustPolicy {
 public:
  bool trusted(const rc::control::Hello&) override { return true; }
};
#endif

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
  rcapp::SessionStatus sessionStatus;
#if defined(RC_APP_E2E_HOST)
  InsecureE2ETrustPolicy trustPolicy;
#else
  rcbackend::RejectingTrustPolicy trustPolicy;
#endif
  rcbackend::SessionConfig sessionConfig;
  sessionConfig.serverName = discovery.computerName().toStdString();
  sessionConfig.serviceId = discovery.serviceID().toStdString();
#if defined(RC_APP_E2E_HOST)
  for (int index = 1; index < argc; ++index) {
    if (std::string(argv[index]) == "--e2e-hevc") {
      sessionConfig.initialStream.codec = rc::control::Codec::Hevc;
    }
  }
#endif
  rcbackend::SessionController sessionHandler(sessionConfig, trustPolicy, nullptr,
                                              &sessionStatus);
  rcnet::TcpListener listener;
  bool testLoopback = false;
#if defined(RC_APP_E2E_HOST)
  testLoopback = true;
#else
  for (int index = 1; index < argc; ++index) {
    if (std::string(argv[index]) == "--test-loopback") testLoopback = true;
  }
#endif
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
  const HRESULT listenerHr =
      listener.start(rcnet::kDefaultPort, &sessionHandler, testLoopback);
  if (SUCCEEDED(listenerHr)) {
    if (testLoopback) discovery.setTestLoopback();
    else discovery.start();
  } else {
    RC_ERR(L"TCP listener could not start: %s", rcwin::hrMessage(listenerHr).c_str());
    discovery.setReceiverFailure(QString::fromStdWString(rcwin::hrMessage(listenerHr)));
  }

  QQmlApplicationEngine engine;
  engine.rootContext()->setContextProperty(QStringLiteral("frameProducer"), &producer);
  engine.rootContext()->setContextProperty(QStringLiteral("lanDiscovery"), &discovery);
  engine.rootContext()->setContextProperty(QStringLiteral("sessionStatus"), &sessionStatus);
#if defined(RC_APP_E2E_HOST)
  engine.rootContext()->setContextProperty(QStringLiteral("appE2EMode"), true);
  const QUrl mainUrl(QStringLiteral("qrc:/qml/Main.qml"));
#else
  engine.rootContext()->setContextProperty(QStringLiteral("appE2EMode"), false);
  const QUrl mainUrl(QStringLiteral("qrc:/RemoteCam/qml/Main.qml"));
#endif
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
