#ifndef RCAPP_SHELL_CONTROLLER_H
#define RCAPP_SHELL_CONTROLLER_H

#include <QObject>

#include <memory>

class QWindow;

namespace rcapp {

// Owns the desktop shell independently of the media pipeline: system tray lifetime,
// explicit Quit, and the opt-in close-to-tray policy. Global hotkeys intentionally
// remain unassigned until the user configures them.
class ShellController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool minimizeToTray READ minimizeToTray WRITE setMinimizeToTray
                 NOTIFY minimizeToTrayChanged)
  Q_PROPERTY(bool trayAvailable READ trayAvailable CONSTANT)

 public:
  ShellController(QObject& mediaOutput, bool enabled, QObject* parent = nullptr);
  ~ShellController() override;

  bool minimizeToTray() const { return minimizeToTray_; }
  bool trayAvailable() const { return trayAvailable_; }
  void setMinimizeToTray(bool enabled);
  void attachWindow(QWindow* window);

 public slots:
  void showWindow();
  void quitApplication();

 signals:
  void minimizeToTrayChanged();

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
  QObject& mediaOutput_;
  QWindow* window_ = nullptr;
  bool minimizeToTray_ = false;
  bool trayAvailable_ = false;
  bool quitting_ = false;
};

}  // namespace rcapp

#endif  // RCAPP_SHELL_CONTROLLER_H
