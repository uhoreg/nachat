#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <unordered_map>
#include <memory>

#include <QMainWindow>

#include "matrix/Matrix.hpp"

#include "ChatWindow.hpp"

class QProgressBar;
class QSettings;
class QLabel;

namespace Ui {
class MainWindow;
}

namespace matrix {
class Room;
}

class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  explicit MainWindow(QSettings &settings, std::unique_ptr<matrix::Session> session);
  ~MainWindow();

signals:
  void quit();

private:
  Ui::MainWindow *ui;
  QSettings &settings_;
  std::unique_ptr<matrix::Session> session_;
  QProgressBar *progress_;
  QLabel *sync_label_;
  ChatWindow *last_focused_ = nullptr;

  std::unordered_map<matrix::Room *, ChatWindow *> chat_windows_;

  void update_rooms();
  void sync_progress(qint64 received, qint64 total);
};

#endif // MAINWINDOW_H