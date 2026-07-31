#include "shell_controller.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QEvent>
#include <QMenu>
#include <QMetaObject>
#include <QSettings>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QWindow>

namespace rcapp {

class ShellController::Impl {
 public:
  std::unique_ptr<QMenu> menu;
  std::unique_ptr<QSystemTrayIcon> tray;
};

ShellController::ShellController(QObject& mediaOutput, bool enabled, QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>()), mediaOutput_(mediaOutput) {
  QSettings settings;
  minimizeToTray_ = settings.value(QStringLiteral("shell/minimizeToTray"), false).toBool();
  trayAvailable_ = enabled && QSystemTrayIcon::isSystemTrayAvailable();
  if (!trayAvailable_) return;

  impl_->menu = std::make_unique<QMenu>();
  QAction* showAction = impl_->menu->addAction(QStringLiteral("Show RemoteCam"));
  QAction* screenshotAction = impl_->menu->addAction(QStringLiteral("Save screenshot"));
  impl_->menu->addSeparator();
  QAction* quitAction = impl_->menu->addAction(QStringLiteral("Quit"));
  connect(showAction, &QAction::triggered, this, &ShellController::showWindow);
  connect(screenshotAction, &QAction::triggered, this, [this] {
    QMetaObject::invokeMethod(&mediaOutput_, "takeScreenshot", Qt::QueuedConnection);
  });
  connect(quitAction, &QAction::triggered, this, &ShellController::quitApplication);

  impl_->tray = std::make_unique<QSystemTrayIcon>();
  impl_->tray->setIcon(QApplication::style()->standardIcon(QStyle::SP_ComputerIcon));
  impl_->tray->setToolTip(QStringLiteral("RemoteCam"));
  impl_->tray->setContextMenu(impl_->menu.get());
  connect(impl_->tray.get(), &QSystemTrayIcon::activated, this,
          [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::DoubleClick || reason == QSystemTrayIcon::Trigger) {
              showWindow();
            }
          });
  impl_->tray->show();
}

ShellController::~ShellController() {
  if (window_ != nullptr) window_->removeEventFilter(this);
}

void ShellController::setMinimizeToTray(bool enabled) {
  if (!trayAvailable_) enabled = false;
  if (minimizeToTray_ == enabled) return;
  minimizeToTray_ = enabled;
  QSettings().setValue(QStringLiteral("shell/minimizeToTray"), minimizeToTray_);
  emit minimizeToTrayChanged();
}

void ShellController::attachWindow(QWindow* window) {
  if (window_ == window) return;
  if (window_ != nullptr) window_->removeEventFilter(this);
  window_ = window;
  if (window_ != nullptr) window_->installEventFilter(this);
}

void ShellController::showWindow() {
  if (window_ == nullptr) return;
  window_->show();
  window_->raise();
  window_->requestActivate();
}

void ShellController::quitApplication() {
  quitting_ = true;
  if (impl_->tray) impl_->tray->hide();
  QCoreApplication::quit();
}

bool ShellController::eventFilter(QObject* watched, QEvent* event) {
  if (watched == window_ && event->type() == QEvent::Close && minimizeToTray_ &&
      trayAvailable_ && !quitting_) {
    static_cast<QCloseEvent*>(event)->ignore();
    window_->hide();
    if (impl_->tray) {
      impl_->tray->showMessage(QStringLiteral("RemoteCam is still running"),
                               QStringLiteral("Use the tray menu to show or quit RemoteCam."),
                               QSystemTrayIcon::Information, 2500);
    }
    return true;
  }
  return QObject::eventFilter(watched, event);
}

}  // namespace rcapp
