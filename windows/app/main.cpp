#include <windows.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QUrl>
#include <cstdlib>

#include "bonjour_advertiser.h"
#include "frame_producer.h"
#include "rcwin/hr.h"

namespace {

constexpr wchar_t kProducerMutexName[] = L"Local\\RemoteCam.QtProducer.Single";

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
  discovery.start();

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
  producer.stop();
  if (producerMutex) {
    ::ReleaseMutex(producerMutex);
    ::CloseHandle(producerMutex);
  }
  return result;
}
