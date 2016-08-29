#ifndef NATIVE_CLIENT_MEMBER_LIST_HPP_
#define NATIVE_CLIENT_MEMBER_LIST_HPP_

#include <map>

#include <QListWidget>

#include "matrix/ID.hpp"

namespace matrix {
class RoomState;
enum class Membership;
}

class MemberList : public QListWidget {
public:
  MemberList(const matrix::RoomState &, QWidget *parent = nullptr);

  void member_display_changed(const matrix::RoomState &, const matrix::UserID &, const QString &old);
  void membership_changed(const matrix::RoomState &, const matrix::UserID &, matrix::Membership old, matrix::Membership current);

  QSize sizeHint() const override;

private:
  class Compare {
  public:
    bool operator()(const QString &a, const QString &b) const { return key(a) < key(b); }

  private:
    static QString key(const QString &n);
  };

  std::map<QString, const matrix::UserID, Compare> members_;
  QSize size_hint_;             // cache to avoid recomputing

  void update_members();
};

#endif
