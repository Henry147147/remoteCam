#include <windows.h>

#include <QCoreApplication>
#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QUrl>
#include <QWindow>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "bonjour_advertiser.h"
#if defined(RC_APP_E2E_HOST)
#include "frame_producer.h"
#else
#include "live_media_pipeline.h"
#include "preview_provider.h"
#endif
#if !defined(RC_APP_E2E_HOST)
#include "security_policy.h"
#endif
#include "session_status.h"
#include "shell_controller.h"
#include "phone_controller.h"
#include "rcbackend/session_controller.h"
#include "rcnet/tcp_listener.h"
#include "rcwin/hr.h"

namespace {

constexpr wchar_t kProducerMutexName[] = L"Local\\RemoteCam.QtProducer.Single";

DWORD runRegistrationHelper(const std::wstring& arguments) {
  const std::filesystem::path appPath = rcwin::modulePath();
  const std::filesystem::path helperPath = appPath.parent_path() / L"rc-vcam-register.exe";
  if (::GetFileAttributesW(helperPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
    RC_ERR(L"startup registration helper is missing: %s", helperPath.c_str());
    return ERROR_FILE_NOT_FOUND;
  }

  std::wstring command = L"\"" + helperPath.wstring() + L"\" " + arguments;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!::CreateProcessW(helperPath.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, appPath.parent_path().c_str(), &startup,
                        &process)) {
    const DWORD error = ::GetLastError();
    RC_ERR(L"could not start registration helper (%s): %s", arguments.c_str(),
           rcwin::hrMessage(HRESULT_FROM_WIN32(error)).c_str());
    return error;
  }

  ::WaitForSingleObject(process.hProcess, INFINITE);
  DWORD exitCode = ERROR_GEN_FAILURE;
  if (!::GetExitCodeProcess(process.hProcess, &exitCode)) exitCode = ::GetLastError();
  ::CloseHandle(process.hThread);
  ::CloseHandle(process.hProcess);
  return exitCode;
}

void ensureMachineIntegration() {
  if (runRegistrationHelper(L"--status") != ERROR_SUCCESS) {
    const DWORD registerExit = runRegistrationHelper(L"--register");
    if (registerExit != ERROR_SUCCESS) {
      RC_ERR(L"automatic virtual-camera registration failed with exit code %lu", registerExit);
    }
  }

  const std::wstring firewallArguments =
      L"--firewall-add --app \"" + rcwin::modulePath() + L"\"";
  const DWORD firewallExit = runRegistrationHelper(firewallArguments);
  if (firewallExit != ERROR_SUCCESS) {
    RC_ERR(L"automatic firewall configuration failed with exit code %lu", firewallExit);
  }
}

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
  QApplication app(argc, argv);
  QGuiApplication::setApplicationName(QStringLiteral("RemoteCam"));
  QGuiApplication::setOrganizationName(QStringLiteral("RemoteCam"));
  rcwin::logInit(L"rc-app");
  ensureMachineIntegration();

  // FrameRing has a single-writer contract. Local\ is deliberate: this
  // coordinates ordinary app instances in the current user's session without
  // requiring the SeCreateGlobalPrivilege that an unelevated desktop app does
  // not possess.
  HANDLE producerMutex = ::CreateMutexW(nullptr, TRUE, kProducerMutexName);
  const DWORD mutexError = ::GetLastError();
  const bool producerConflict = producerMutex && mutexError == ERROR_ALREADY_EXISTS;

#if defined(RC_APP_E2E_HOST)
  rcapp::FrameProducer producer;
#else
  rcapp::LiveMediaPipeline producer;
#endif
  rcapp::BonjourAdvertiser discovery;
  rcapp::SessionStatus sessionStatus;
#if defined(RC_APP_E2E_HOST)
  rcapp::ShellController shellController(producer, false);
#else
  rcapp::ShellController shellController(producer, true);
#endif
#if defined(RC_APP_E2E_HOST)
  // Unconditional, so the desktop harness never depends on a user setting.
  InsecureE2ETrustPolicy trustPolicy;
#else
  rcapp::SecurityPolicy trustPolicy;
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
  rcbackend::SessionController sessionHandler(
      sessionConfig, trustPolicy,
#if defined(RC_APP_E2E_HOST)
      nullptr,
#else
      &producer,
#endif
      &sessionStatus);
  rcapp::PhoneController phoneController(sessionHandler);
#if !defined(RC_APP_E2E_HOST)
  producer.setKeyframeRequester([&sessionHandler] { sessionHandler.requestKeyframe(); });
#endif
  rcnet::TcpListener listener;
  bool testLoopback = false;
#if !defined(RC_APP_E2E_HOST)
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
#if !defined(RC_APP_E2E_HOST)
  auto previewProvider = std::make_unique<rcapp::PreviewProvider>();
  producer.setPreviewProvider(previewProvider.get());
  engine.addImageProvider(QStringLiteral("live"), previewProvider.release());
#endif
  engine.rootContext()->setContextProperty(QStringLiteral("frameProducer"), &producer);
  engine.rootContext()->setContextProperty(QStringLiteral("lanDiscovery"), &discovery);
  engine.rootContext()->setContextProperty(QStringLiteral("sessionStatus"), &sessionStatus);
  engine.rootContext()->setContextProperty(QStringLiteral("shellController"), &shellController);
  engine.rootContext()->setContextProperty(QStringLiteral("phoneController"), &phoneController);
#if defined(RC_APP_E2E_HOST)
  // The E2E host bypasses trust unconditionally and has no user-facing setting to bind.
  engine.rootContext()->setContextProperty(QStringLiteral("securityPolicy"),
                                           static_cast<QObject*>(nullptr));
  engine.rootContext()->setContextProperty(QStringLiteral("appE2EMode"), true);
  const QUrl mainUrl(QStringLiteral("qrc:/qml/Main.qml"));
#else
  engine.rootContext()->setContextProperty(QStringLiteral("securityPolicy"), &trustPolicy);
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
  if (!engine.rootObjects().isEmpty()) {
    shellController.attachWindow(qobject_cast<QWindow*>(engine.rootObjects().constFirst()));
  }

  const int result = app.exec();
  listener.stop();
  producer.stop();
  if (producerMutex) {
    ::ReleaseMutex(producerMutex);
    ::CloseHandle(producerMutex);
  }
  return result;
}
