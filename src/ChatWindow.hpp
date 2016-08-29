#ifndef NATIVE_CHAT_CHAT_WINDOW_HPP_
#define NATIVE_CHAT_CHAT_WINDOW_HPP_

#include <unordered_map>
#include <memory>

#include <QWidget>

#include "matrix/Room.hpp"

class RoomView;
class RoomViewList;
class ThumbnailCache;

namespace matrix {
class Room;
}

namespace Ui {
class ChatWindow;
}

class ChatWindow : public QWidget
{
  Q_OBJECT

public:
  explicit ChatWindow(ThumbnailCache &cache, QWidget *parent = 0);
  ~ChatWindow();

  void add(matrix::Room &r, RoomView *); // Takes ownership
  void add_or_focus(matrix::Room &);
  void room_display_changed(matrix::Room &);

  RoomView *take(const matrix::RoomID &); // Releases ownership

  const matrix::RoomID &focused_room() const;

signals:
  void focused(const matrix::RoomID &);
  void released(const matrix::RoomID &);
  void claimed(const matrix::RoomID &);
  void pop_out(const matrix::RoomID &, RoomView *);

protected:
  void changeEvent(QEvent *event) override;
  void closeEvent(QCloseEvent *event) override;

private:
  Ui::ChatWindow *ui;
  RoomViewList *room_list_;
  std::unordered_map<matrix::RoomID, RoomView *> rooms_;
  ThumbnailCache &cache_;

  void update_title();
  void current_changed(int i);
};

#endif
