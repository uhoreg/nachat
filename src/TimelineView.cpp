#include "TimelineView.hpp"

#include <cmath>
#include <vector>
#include <sstream>
#include <iomanip>
#include <stdexcept>

#include <QShortcut>
#include <QPainter>
#include <QScrollBar>
#include <QRegularExpression>
#include <QStringBuilder>
#include <QGuiApplication>
#include <QClipboard>
#include <QToolTip>
#include <QStyleHints>
#include <QMenu>
#include <QDesktopServices>

#include <QDebug>

#include "matrix/Room.hpp"

#include "Spinner.hpp"
#include "RedactDialog.hpp"
#include "EventSourceView.hpp"

using std::experimental::optional;

namespace {

constexpr std::chrono::minutes BLOCK_MERGE_INTERVAL(5);

qreal block_spacing(const QWidget &parent) {
  return std::round(parent.fontMetrics().lineSpacing() * 0.75);
}

qreal block_padding(const QWidget &parent) {
  return std::round(parent.fontMetrics().lineSpacing() * 0.33);
}

auto to_time_point(uint64_t ts) {
  return std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::system_clock::from_time_t(0))
      + std::chrono::duration<uint64_t, std::milli>(ts);
}

QString to_timestamp(const char *format, Time p) {
  auto time = std::chrono::system_clock::to_time_t(p);
  auto tm = std::localtime(&time);
  std::ostringstream s;
  s << std::put_time(tm, format);
  return QString::fromStdString(s.str());
}

QString pretty_size(double n) {
  constexpr const static char *const units[9] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB", "EiB", "ZiB", "YiB"}; // should be enough for anyone!
  auto idx = std::min<size_t>(8, std::log(n)/std::log(1024.));
  return QString::number(n / std::pow<double>(1024, idx), 'g', 4) + " " + units[idx];
}

void href_urls(const QPalette &palette, QVector<QTextLayout::FormatRange> &formats, const QString &text, int offset = 0) {
  const static QRegularExpression regex(
    R"(\b()"
    R"([a-z][a-z0-9+-.]*://[^\s]+)"
    R"(|[^\s]+\.(com|net|org)(/[^\s]*)?)"
    R"(|www\.[^\s]+\.[^\s]+)"
    R"(|data:[^\s]+)"
    R"())",
    QRegularExpression::UseUnicodePropertiesOption | QRegularExpression::OptimizeOnFirstUsageOption | QRegularExpression::CaseInsensitiveOption);

  auto urls = regex.globalMatch(text, offset);
  while(urls.hasNext()) {
    auto candidate = urls.next();
    // QUrl doesn't handle some things consistently (e.g. emoticons in .la) so we round-trip it
    QUrl url(QUrl(candidate.captured(), QUrl::StrictMode).toString(QUrl::FullyEncoded), QUrl::StrictMode);
    if(!url.isValid()) continue;
    if(url.scheme().isEmpty()) url = QUrl("http://" + url.toString(QUrl::FullyEncoded), QUrl::StrictMode);

    QTextLayout::FormatRange range;
    range.start = candidate.capturedStart();
    range.length = candidate.capturedLength();
    QTextCharFormat format;
    format.setAnchor(true);
    format.setAnchorHref(url.toString(QUrl::FullyEncoded));
    format.setForeground(palette.link());
    format.setFontUnderline(true);
    range.format = format;
    formats.push_back(range);
  }
}

QVector<QTextLayout::FormatRange> format_view(const QVector<QTextLayout::FormatRange> &formats, int start, int length) {
  const auto end = start + length;
  QVector<QTextLayout::FormatRange> result;
  for(const auto &in : formats) {
    const auto in_end = in.start + in.length;
    if(in_end <= start || end <= in.start) continue;
    QTextLayout::FormatRange out;
    out.format = in.format;
    out.start = in.start - start;
    out.length = in.length;
    result.push_back(out);
  }
  return result;
}

optional<matrix::UserID> get_affected_user(const matrix::event::Room &e) {
  if(e.type() != matrix::event::room::Member::tag()) return {};
  matrix::event::room::Member member_evt{matrix::event::room::State{e}};
  return member_evt.user();
}

optional<matrix::EventID> get_redacts(const matrix::event::Room &e) {
  if(e.type() != matrix::event::room::Redaction::tag()) return {};
  matrix::event::room::Redaction re{e};
  return re.redacts();
}

void populate_menu_href(QMenu &menu, const QUrl &homeserver, const QString &href) {
  menu.addSection(TimelineView::tr("Link"));
  const QUrl url(href);
  if(url.scheme() == "mxc") {
    auto http_action = menu.addAction(QIcon::fromTheme("edit-copy"), TimelineView::tr("&Copy link HTTP address"));
    auto hurl = matrix::Content(url).url_on(homeserver).toString(QUrl::FullyEncoded);
    QObject::connect(http_action, &QAction::triggered, [=]() {
        QGuiApplication::clipboard()->setText(hurl);
        QGuiApplication::clipboard()->setText(hurl, QClipboard::Selection);
      });
  }
  auto copy_action = menu.addAction(QIcon::fromTheme("edit-copy"), url.scheme() == "mxc" ? TimelineView::tr("Copy link &MXC address") : TimelineView::tr("&Copy link address"));
  QObject::connect(copy_action, &QAction::triggered, [=]() {
      QGuiApplication::clipboard()->setText(href);
      QGuiApplication::clipboard()->setText(href, QClipboard::Selection);
    });
}

void populate_menu_event(QMenu &menu, TimelineView &view, const matrix::event::Room &event) {
  menu.addSection(TimelineView::tr("Event"));
  auto redact_action = menu.addAction(QIcon::fromTheme("edit-delete"), TimelineView::tr("&Redact..."));
  const auto event_id = event.id();
  QObject::connect(redact_action, &QAction::triggered, [&view, event_id]() {
      auto dialog = new RedactDialog(&view);
      dialog->setAttribute(Qt::WA_DeleteOnClose);
      QObject::connect(dialog, &QDialog::accepted, [&view, dialog, event_id]() {
          view.redact_requested(event_id, dialog->reason());
        });
      dialog->open();
    });

  auto source_action = menu.addAction(TimelineView::tr("&View source..."));
  const QJsonObject source = event.json();
  QObject::connect(source_action, &QAction::triggered, [source]() {
      (new EventSourceView(source))->show();
    });
}

}

EventLike::EventLike(TimelineEventID id, const matrix::RoomState &state, matrix::event::Room real)
  : EventLike(id, state, real.sender(), to_time_point(real.origin_server_ts()), real.type(), real.content(), get_affected_user(real), get_redacts(real))
{
  event = std::move(real);
}

EventLike::EventLike(TimelineEventID id, const matrix::RoomState &state,
                     const matrix::UserID &sender, Time time, matrix::EventType type, matrix::event::Content content,
                     optional<matrix::UserID> affected_user, optional<matrix::EventID> redacts)
  : id{id}, type{std::move(type)}, time{time}, sender{sender}, redacts{redacts}, content{std::move(content)}
{
  if(affected_user) {
    auto m = state.member_from_id(*affected_user);
    affected_user_info = MemberInfo{*affected_user, m ? *m : matrix::event::room::MemberContent::leave};
  }

  auto member = state.member_from_id(sender);
  if(member) {
    disambiguation = state.member_disambiguation(sender);
    member_content = *member;
  }
}

optional<matrix::event::room::MemberContent> EventLike::effective_profile() const {
  // Events concerning non-present users use the profile they set, whereas all others use the previously set one, if any
  if(affected_user_info && affected_user_info->user == sender) {
    matrix::event::room::MemberContent mc{content};
    if(affected_user_info->prev_content.membership() == matrix::Membership::LEAVE
       || affected_user_info->prev_content.membership() == matrix::Membership::BAN) {
      return mc;
    }
  }
  return member_content;
}

void EventLike::redact(const matrix::event::room::Redaction &because) {
  if(!event) throw std::logic_error("tried to redact a fake event");
  event->redact(because);
  time = {};
  content = event->content();
}

EventBlock::EventBlock(TimelineView &parent, ThumbnailCache &thumbnail_cache, gsl::span<const EventLike *const> events)
  : parent_{parent}, sender_{events[0]->sender}, events_{static_cast<std::size_t>(events.size())}
{
  const auto &front = *events[0];

  if(auto p = front.effective_profile()) {
    if(auto avatar = p->avatar_url()) {
      const auto size = static_cast<int>(std::floor(avatar_extent()));
      try {
        avatar_ = ThumbnailRef{matrix::Thumbnail{matrix::Content{*avatar}, QSize{size, size}, matrix::ThumbnailMethod::SCALE}, thumbnail_cache};
      } catch(const matrix::illegal_content_scheme &) {
        qDebug() << "illegal content in avatar url" << *avatar << "for user" << front.sender.value();
      }
    }
  }

  if(front.time) {
    time_ = TimeInfo{*events[0]->time, *events[events.size()-1]->time};
  }

  {
    QTextOption options;
    options.setAlignment(Qt::AlignLeft | Qt::AlignTop);
    options.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);

    optional<QString> displayname;
    if(auto p = front.effective_profile()) {
      displayname = p->displayname();
    }
    name_.setText((displayname ? *displayname : front.sender.value())
                  + (front.disambiguation ? QString(" (" % *front.disambiguation % ")") : ""));
    name_.setFont(parent_.font());
    name_.setTextOption(options);
    name_.setCacheEnabled(true);
  }

  {
    QTextOption options;
    options.setAlignment(Qt::AlignRight | Qt::AlignTop);
    options.setWrapMode(QTextOption::NoWrap);
    timestamp_.setFont(parent_.font());
    timestamp_.setTextOption(options);
    timestamp_.setCacheEnabled(true);
  }

  for(std::size_t i = 0; i < static_cast<size_t>(events.size()); ++i) {
    events_.emplace_back(parent, *this, *events[i]);
  }
}

void EventBlock::update_layout(qreal width) {
  const auto &metrics = parent_.fontMetrics();

  // Header and first line
  const qreal early_offset = avatar_extent() + horizontal_padding();

  size_t lines = 0;
  {
    name_.beginLayout();
    while(true) {
      auto line = name_.createLine();
      if(!line.isValid()) break;
      qreal offset = (lines < 2) * early_offset;
      line.setLineWidth(width - offset);
      line.setPosition(QPointF(offset, lines * metrics.lineSpacing()));
      lines += 1;
    }
    name_.endLayout();
  }

  {
    // Lay out as a single or range timestamp as appropriate, degrading to single or nothing of space in the header is unavailable
    auto layout_ts = [&](){
      timestamp_.beginLayout();
      auto line = timestamp_.createLine();
      line.setLineWidth(width - early_offset);
      line.setPosition(QPointF(early_offset, 0));
      timestamp_.endLayout();
      if(name_.lineAt(0).naturalTextWidth() + early_offset > width - line.naturalTextWidth()) {
        timestamp_.clearLayout();
        return false;
      }
      return true;
    };
    if(time_) {
      auto start_ts = to_timestamp("%H:%M", time_->start);
      bool done = false;
      if(time_->end - time_->start > BLOCK_MERGE_INTERVAL) {
        auto end_ts = to_timestamp("%H:%M", time_->end);
        timestamp_.setText(start_ts % "–" % end_ts);
        done = layout_ts();
      }
      if(!done) {
        timestamp_.setText(start_ts);
        layout_ts();
      }
    } else {
      timestamp_.setText(TimelineView::tr("REDACTED"));
      layout_ts();
    }
  }

  for(auto &event : events_) {
    for(auto &paragraph : event.paragraphs) {
      paragraph.beginLayout();
      while(true) {
        auto line = paragraph.createLine();
        if(!line.isValid()) break;
        qreal offset = (lines < 2) * early_offset;
        line.setLineWidth(width - offset);
        line.setPosition(QPointF(offset, lines * metrics.lineSpacing()));
        lines += 1;
      }
      paragraph.endLayout();
    }
  }
}

QRectF EventBlock::bounds() const {
  // We assume that name_ overlaps timestamp_ and that all paragraphs have equal width.
  //return QRectF(0, 0, avatar_extent(), avatar_extent()) | name_.boundingRect() | events_.back().paragraphs.back().boundingRect();
  size_t lines = name_.lineCount();
  for(const auto &event : events_) {
    for(const auto &paragraph : event.paragraphs) {
      lines += paragraph.lineCount();
    }
  }
  return QRectF(0, 0,
                avatar_extent() + horizontal_padding() + name_.boundingRect().width(),
                std::max<qreal>(avatar_extent(), (std::max<size_t>(2, lines) - 1) * parent_.fontMetrics().lineSpacing() + parent_.fontMetrics().ascent()));
}

struct TextRange {
  int start, length;
};

QTextLayout::FormatRange to_selection_format(const TextRange &range, const QPalette &palette, bool focused = true) {
  QTextCharFormat selected;
  {
    const auto state = focused ? QPalette::Active : QPalette::Inactive;
    selected.setBackground(palette.brush(state, QPalette::Highlight));
    selected.setForeground(palette.brush(state, QPalette::HighlightedText));
  }

  QTextLayout::FormatRange result;
  result.format = selected;
  result.start = range.start;
  result.length = range.length;
  return result;
}

struct SelectionResult {
  bool continues;
  TextRange affected;
};

static bool cursor_in(const Cursor &c, TimelineEventID id, Cursor::Type type, size_t paragraph) {
  return c.event() == id && c.type() == type && c.paragraph() == paragraph;
}

static optional<SelectionResult> selection_for(TimelineEventID id, Cursor::Type type, const QTextLayout &layout, bool bottom_selected, const Selection &selection, size_t paragraph = 0) {
  optional<SelectionResult> result;

  const bool begin_applies = cursor_in(selection.begin, id, type, paragraph);
  const bool end_applies = cursor_in(selection.end, id, type, paragraph);
  if(begin_applies && end_applies) {
    result = SelectionResult{};
    result->affected.start = std::min(selection.begin.pos(), selection.end.pos());
    result->affected.length = std::max(selection.begin.pos(), selection.end.pos()) - result->affected.start;
    result->continues = false;
  } else if(begin_applies || end_applies) {
    result = SelectionResult{};
    const int endpoint = (begin_applies ? selection.begin : selection.end).pos();
    if(bottom_selected) {
      result->affected.start = std::max(0, endpoint);
      result->affected.length = layout.text().size() - result->affected.start;
      result->continues = false;
    } else {
      result->affected.start = 0;
      result->affected.length = std::min(layout.text().size(), endpoint);
      result->continues = true;
    }
  } else if(bottom_selected) {
    result = SelectionResult{};
    result->affected.start = 0;
    result->affected.length = layout.text().size();
    result->continues = true;
  }

  if(result) {
    switch(selection.mode) {
    case Selection::Mode::CHARACTER: break;
    case Selection::Mode::WORD:
      result->affected.length = layout.nextCursorPosition(result->affected.start + result->affected.length, QTextLayout::SkipWords);
      result->affected.start = layout.previousCursorPosition(result->affected.start, QTextLayout::SkipWords);
      break;
    case Selection::Mode::PARAGRAPH:
      result->affected.start = 0;
      result->affected.length = layout.text().size();
      break;
    }
  }

  return result;
}

bool EventBlock::draw(QPainter &p, bool bottom_selected, const Selection &selection) const {
  if(avatar_) {
    if(const auto &pixmap = **avatar_) {
      const QSize logical_size = pixmap->size() / pixmap->devicePixelRatio();
      p.drawPixmap(QPointF((avatar_extent() - logical_size.width()) * 0.5,
                           (avatar_extent() - logical_size.height()) * 0.5),
                   *pixmap);
    } else {
      // TODO: Draw loading or error indicator
    }
  } else {
    // TODO: Draw default avatar
  }

  const static QPointF origin{0, 0};
  
  QVector<QTextLayout::FormatRange> selections;
  for(auto event = events_.rbegin(); event != events_.rend(); ++event) {
    size_t index = event->paragraphs.size();
    for(auto paragraph = event->paragraphs.rbegin(); paragraph != event->paragraphs.rend(); ++paragraph) {
      index -= 1;
      if(auto s = selection_for(event->id, Cursor::Type::BODY, *paragraph, bottom_selected, selection, index)) {
        selections.push_back(to_selection_format(s->affected, parent_.palette(), parent_.hasFocus()));
        bottom_selected = s->continues;
      }
      paragraph->draw(&p, origin, selections);
      selections.clear();
    }
  }

  if(auto s = selection_for(events_.front().id, Cursor::Type::TIMESTAMP, timestamp_, bottom_selected, selection)) {
    selections.push_back(to_selection_format(s->affected, parent_.palette(), parent_.hasFocus()));
    bottom_selected = s->continues;
  }
  timestamp_.draw(&p, origin, selections);
  selections.clear();

  if(auto s = selection_for(events_.front().id, Cursor::Type::NAME, name_, bottom_selected, selection)) {
    selections.push_back(to_selection_format(s->affected, parent_.palette(), parent_.hasFocus()));
    bottom_selected = s->continues;
  }
  name_.draw(&p, origin, selections);
  selections.clear();
  return bottom_selected;
}

void EventBlock::handle_input(const QPointF &point, QEvent *input) {
  const QRectF avatar_rect(0, 0, avatar_extent(), avatar_extent());

  switch(input->type()) {
  case QEvent::MouseButtonPress: {
    const auto cursor = get_cursor(point, true);
    if(cursor && cursor->href) {
      input->accept();
    } else {
      input->ignore();
    }
    break;
  }
  case QEvent::MouseButtonRelease: {
    const auto cursor = get_cursor(point, true);
    if(cursor && cursor->href) {
      input->accept();
      QUrl url(*cursor->href);
      if(url.scheme() == "mxc") {
        url = matrix::Content{url}.url_on(parent_.homeserver());
      }
      if(!QDesktopServices::openUrl(url)) {
        qDebug() << "failed to open URL" << url.toString(QUrl::FullyEncoded);
      }
    } else {
      input->ignore();
    }
    break;
  }
  case QEvent::MouseMove: {
    const auto cursor = get_cursor(point, true);
    if(cursor) {
      if(cursor->href) {
        parent_.setCursor(Qt::PointingHandCursor);
      } else {
        parent_.setCursor(Qt::IBeamCursor);
      }
      input->accept();
    } else {
      parent_.setCursor(Qt::ArrowCursor);
    }
    break;
  }
  case QEvent::ContextMenu: {
    auto menu = new QMenu(&parent_);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    const auto event = event_at(point);
    if(event && event->source) {
      populate_menu_event(*menu, parent_, *event->source);
    }

    if(avatar_ && avatar_rect.contains(point)) {
      populate_menu_href(*menu, parent_.homeserver(), avatar_->content().content().url().toString(QUrl::FullyEncoded));
    } else {
      const auto cursor = get_cursor(point, true);
      if(cursor && cursor->href) {
        populate_menu_href(*menu, parent_.homeserver(), *cursor->href);
      }
    }

    menu->addSection(TimelineView::tr("User"));
    auto profile_action = menu->addAction(QIcon::fromTheme("user-available"), TimelineView::tr("View &profile..."));
    QObject::connect(profile_action, &QAction::triggered, [this]() {
        parent_.view_user_profile(sender_);
      });

    menu->popup(static_cast<QContextMenuEvent*>(input)->globalPos());
    break;
  }
  case QEvent::ToolTip: {
    auto help = static_cast<QHelpEvent*>(input);
    QString message;
    if(timestamp_.lineCount() != 0 && timestamp_.lineAt(0).naturalTextRect().contains(point)) {
      const auto &event = events_.front();
      if(event.time) {
        message = to_timestamp("%Y-%m-%d %H:%M:%S", *event.time);
      }
    } else if(avatar_rect.contains(point) || name_.boundingRect().contains(point)) {
      message = sender_.value();
    } else {
      const auto event = event_at(point);
      if(event && event->source) {
        if(!event->source->redacted()) {
          message = to_timestamp("%Y-%m-%d %H:%M:%S", to_time_point(event->source->origin_server_ts()));
        }
      } else if(event) {
        message = TimelineView::tr("Sending...");
      }
    }
    if(!message.isEmpty()) {
      QToolTip::showText(help->globalPos(), message);
    } else {
      input->ignore();
    }
    break;
  }
  default:
    input->ignore();
    break;
  }
}

qreal EventBlock::avatar_extent() const {
  const auto m = parent_.fontMetrics();
  // From 0 to baseline of second line of text, so text flowed underneath isn't cramped
  return m.lineSpacing() + m.ascent();
}

qreal EventBlock::horizontal_padding() const {
  return std::round(parent_.fontMetrics().lineSpacing() * 0.33);
}

const EventBlock::Event *EventBlock::event_at(const QPointF &point) const {
  for(const auto &event : events_) {
    QRectF bounds;
    for(const auto &paragraph : event.paragraphs) {
      bounds |= paragraph.boundingRect();
    }
    if(bounds.contains(point)) return &event;
  }
  return nullptr;
}

static optional<int> cursor_near(const QTextLayout &layout, const QPointF &p, bool exact) {
  for(int i = 0; i < layout.lineCount(); ++i) {
    const auto line = layout.lineAt(i);
    const auto rect = line.rect();
    if(p.y() < rect.top()) {
      if(exact) return {};
      return line.xToCursor(rect.left());
    }
    if(p.y() >= rect.top() && p.y() <= rect.bottom()) {
      if(exact && (p.x() < line.x() || p.x() > (line.x() + line.naturalTextWidth()))) return {};
      return line.xToCursor(p.x());
    }
  }

  if(exact) return {};
  auto line = layout.lineAt(layout.lineCount()-1);
  return line.xToCursor(line.rect().right());
}

static optional<QString> href_at(const QTextLayout &layout, int cursor) {
  for(auto &format : layout.formats()) {
    if(format.start <= cursor && format.start + format.length > cursor && format.format.isAnchor()) {
      return format.format.anchorHref();
    }
  }
  return {};
}

optional<CursorWithHref> EventBlock::get_cursor(const QPointF &point, bool exact) const {
  auto header_rect = name_.boundingRect();
  if(point.y() < header_rect.bottom()) {
    if(timestamp_.lineCount() != 0) {
      const auto line = timestamp_.lineAt(0);
      const auto rect = line.naturalTextRect();
      if(point.x() > rect.left() && point.y() > rect.top() && point.y() < rect.bottom()) { // TODO: LTR support?
        return CursorWithHref{Cursor{Cursor::Type::TIMESTAMP, events_.front().id, timestamp_.lineAt(0).xToCursor(point.x())}, {}};
      }
    } 
    if(auto c = cursor_near(name_, point, exact)) {
      return CursorWithHref{Cursor{Cursor::Type::NAME, events_.front().id, *c}, {}};
    }
  }

  for(const auto &event : events_) {
    size_t index = 0;
    for(const auto &paragraph : event.paragraphs) {
      const auto paragraph_rect = paragraph.boundingRect();

      if(point.y() <= paragraph_rect.bottom()) {
        if(auto c = cursor_near(paragraph, point, exact)) {
          return CursorWithHref{Cursor{event.id, index, *c}, href_at(paragraph, *c)};
        }
      }

      ++index;
    }
  }

  if(exact) return {};

  const auto &paragraph = events_.back().paragraphs.back();
  const auto line = paragraph.lineAt(paragraph.lineCount()-1);
  auto c = line.xToCursor(line.x() + line.width());
  return CursorWithHref{Cursor{events_.back().id, events_.back().paragraphs.size() - 1, c}, href_at(paragraph, c)};
}

EventBlock::SelectionTextResult EventBlock::selection_text(bool bottom_selected, const Selection &selection) const {
  QString result;
  for(auto event = events_.rbegin(); event != events_.rend(); ++event) {
    size_t index = event->paragraphs.size();
    for(auto paragraph = event->paragraphs.rbegin(); paragraph != event->paragraphs.rend(); ++paragraph) {
      index -= 1;
      if(auto s = selection_for(event->id, Cursor::Type::BODY, *paragraph, bottom_selected, selection, index)) {
        result = " " % paragraph->text().mid(s->affected.start, s->affected.length) % "\n" % result;
        bottom_selected = s->continues;
      }
    }
  }

  QString timestamp;
  if(auto s = selection_for(events_.front().id, Cursor::Type::TIMESTAMP, timestamp_, bottom_selected, selection)) {
    timestamp = timestamp_.text().mid(s->affected.start, s->affected.length);
    bottom_selected = s->continues;
  }

  QString name;
  if(auto s = selection_for(events_.front().id, Cursor::Type::NAME, name_, bottom_selected, selection)) {
    name = name_.text().mid(s->affected.start, s->affected.length);
    bottom_selected = s->continues;
  }

  if(!timestamp.isEmpty()) {
    result = timestamp + (result.isEmpty() ? "" : "\n" + result);
  }

  if(!name.isEmpty()) {
    result = name + (result.isEmpty() ? "" : " - " + result);
  }

  return SelectionTextResult{std::move(result), bottom_selected};
}

EventBlock::Event::Event(const TimelineView &view, const EventBlock &block, const EventLike &e)
  : id{e.id}, time{e.time}, source{e.event} {

  const auto &&tr = [](const char *s) { return TimelineView::tr(s); };

  QString text;
  QVector<QTextLayout::FormatRange> formats;

  using namespace matrix::event::room;

  optional<Redaction> redaction;
  if(e.event && e.event->unsigned_data()) {
    redaction = e.event->unsigned_data()->redacted_because();
  }

  const auto &&redaction_note = [&]() {
    if(redaction) {
      if(auto r = redaction->content().reason()) {
        text = tr("%1 (redacted: %2)").arg(text).arg(*r);
      } else {
        text = tr("%1 (redacted)").arg(text);
      }
    }
  };

  if(e.type == Message::tag()) {
    MessageContent msg{e.content};
    if(redaction) {
      if(auto r = redaction->content().reason()) {
        text = tr("REDACTED: %1").arg(*r);
      } else {
        text = tr("REDACTED");
      }
    } else if(msg.type() == message::Text::tag() || msg.type() == message::Notice::tag()) {
      text = msg.body();
      href_urls(view.palette(), formats, text);
    } else if(msg.type() == message::Emote::tag()) {
      text = QString("* %1 %2").arg(block.name_.text()).arg(msg.body());
      href_urls(view.palette(), formats, text, block.name_.text().size() + 3);
    } else {
      qDebug() << "displaying fallback for unrecognized msgtype:" << msg.type().value();
      text = msg.body();
      href_urls(view.palette(), formats, text);
    }
  } else if(e.type == Member::tag()) {
    const MemberContent content{e.content};
    const MemberContent prev_content{e.affected_user_info->prev_content};
    const matrix::UserID &user = e.affected_user_info->user;
    if(user == block.sender_) {
      switch(content.membership()) {
      case matrix::Membership::INVITE:
        text = tr("invited themselves");
        break;
      case matrix::Membership::JOIN:
        switch(prev_content.membership()) {
        case matrix::Membership::INVITE:
          text = tr("accepted invite");
          break;
        case matrix::Membership::JOIN:
          if(content.avatar_url() != prev_content.avatar_url()) {
            if(content.displayname() != prev_content.displayname()) {
              if(content.displayname()) {
                text = tr("changed avatar and set display name to \"%1\"").arg(*content.displayname());
              } else {
                text = tr("changed avatar and removed display name");
              }
            } else {
              text = tr("changed avatar");
            }
          } else if(content.displayname() != prev_content.displayname()) {
            if(content.displayname()) {
              text = tr("set display name to \"%1\"").arg(*content.displayname());
            } else {
              text = tr("removed display name");
            }
          } else {
            text = tr("sent a no-op join");
          }
          break;
        default:
          text = tr("joined");
          break;
        }
        break;
      case matrix::Membership::LEAVE:
        text = tr("left");
        break;
      case matrix::Membership::BAN:
        text = tr("banned themselves");
        break;
      }
    } else {
      const QString pretty_target = content.displayname().value_or(user.value()); // TODO: Clickable
      switch(content.membership()) {
      case matrix::Membership::INVITE:
        text = tr("invited %1").arg(pretty_target);
        break;
      case matrix::Membership::JOIN:
        if(prev_content.membership() == matrix::Membership::JOIN) {
          text = tr("modified profile of %1").arg(pretty_target);
        } else {
          text = tr("forced %1 to join").arg(pretty_target);
        }
        break;
      case matrix::Membership::LEAVE:
        switch(prev_content.membership()) {
        case matrix::Membership::INVITE:
          text = tr("rescinded invite to %1").arg(pretty_target);
          break;
        case matrix::Membership::BAN:
          text = tr("unbanned %1").arg(pretty_target);
          break;
        default:
          text = tr("kicked %1").arg(pretty_target);
          break;
        }
      case matrix::Membership::BAN:
        text = tr("banned %1").arg(pretty_target);
        break;
      }
    }
    redaction_note();
  } else if(e.type == Name::tag()) {
    const auto n = NameContent{e.content}.name();
    if(n) {
      text = tr("set the room name to \"%1\"").arg(*n);
    } else {
      text = tr("removed the room name");
    }
    redaction_note();
  } else if(e.type == Create::tag()) {
    text = tr("created the room");
  } else if(e.type == Redaction::tag()) {
    auto reason = e.content.json()["reason"].toString();
     // TODO: Clickable event ID
    if(reason.isEmpty()) {
      text = tr("redacted %1").arg(e.redacts->value());
    } else {
      text = tr("redacted %1: %2").arg(e.redacts->value()).arg(reason);
    }
  } else {
    text = tr("unrecognized message type %1").arg(e.type.value());
  }

  const static QRegularExpression line_re("\\R", QRegularExpression::UseUnicodePropertiesOption | QRegularExpression::OptimizeOnFirstUsageOption);
  auto lines = text.split(line_re);


  QTextOption body_options;
  body_options.setAlignment(Qt::AlignLeft | Qt::AlignTop);
  body_options.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);

  paragraphs = FixedVector<QTextLayout>(lines.size());
  size_t start = 0;
  for(int i = 0; i < lines.size(); ++i) {
    paragraphs.emplace_back(lines[i], view.font());
    auto &paragraph = paragraphs.back();

    paragraph.setFormats(format_view(formats, start, lines[i].length()));
    paragraph.setTextOption(body_options);
    paragraph.setCacheEnabled(true);

    start += 1 + lines[i].size();
  }
}

TimelineView::TimelineView(const QUrl &homeserver, ThumbnailCache &cache, QWidget *parent)
  : QAbstractScrollArea{parent}, homeserver_{homeserver}, thumbnail_cache_{cache}, selection_updating_{false}, click_count_{0},
    copy_{new QShortcut(QKeySequence::Copy, this)}, at_bottom_{false}, id_counter_{0} {
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  verticalScrollBar()->setSingleStep(20);  // Taken from QScrollArea
  setMouseTracking(true);

  QSizePolicy policy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  policy.setHorizontalStretch(1);
  policy.setVerticalStretch(0);
  setSizePolicy(policy);

  connect(verticalScrollBar(), &QAbstractSlider::valueChanged, [this]() {
      maybe_need_forwards();
      maybe_need_backwards();
    });
  connect(copy_, &QShortcut::activated, this, &TimelineView::copy);

  {
    const int extent = devicePixelRatioF() * spinner_space() * .9;
    spinner_ = QPixmap(extent, extent);
    spinner_.fill(Qt::transparent);
    QPainter painter(&spinner_);
    painter.setRenderHint(QPainter::Antialiasing);
    Spinner::paint(palette().color(QPalette::Shadow), palette().color(QPalette::Base), painter, extent);
    spinner_.setDevicePixelRatio(devicePixelRatioF());
  }
}

void TimelineView::prepend(const matrix::TimelineCursor &begin, const matrix::RoomState &state, const matrix::event::Room &evt) {
  if(!batches_.empty() && batches_.front().begin == begin) {
    batches_.front().events.emplace_front(get_id(), state, evt);
  } else {
    batches_.emplace_front(begin, std::deque<EventLike>{EventLike{get_id(), state, evt}});
  }

  if(auto u = evt.unsigned_data()) {
    if(auto txid = u->transaction_id()) {
      pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                    [&](const Pending &p) { return p.transaction == *txid; }),
                     pending_.end());
    }
  }

  rebuild_blocks();
  maybe_need_backwards();
}

void TimelineView::append(const matrix::TimelineCursor &begin, const matrix::RoomState &state, const matrix::event::Room &evt) {
  optional<TimelineEventID> existing_id;
  if(auto u = evt.unsigned_data()) {
    if(auto txid = u->transaction_id()) {
      std::deque<Pending>::const_iterator it;
      for(it = pending_.cbegin(); it != pending_.cend(); ++it) {
        if(it->transaction == *txid) {
          break;
        }
      }
      existing_id = it->event.id;
      pending_.erase(it);
    }
  }

  const auto id = existing_id ? *existing_id : get_id();
  if(!batches_.empty() && batches_.back().begin == begin) {
    batches_.back().events.emplace_back(id, state, evt);
  } else {
    batches_.emplace_back(begin, std::deque<EventLike>{EventLike{id, state, evt}});
  }

  rebuild_blocks();
  maybe_need_forwards();
}

void TimelineView::redact(const matrix::event::room::Redaction &redaction) {
  bool done = false;
  for(auto &batch : batches_) {
    for(auto &existing_event : batch.events) {
      if(existing_event.event->id() == redaction.redacts()) {
        existing_event.redact(redaction);
        done = true;
      }
      if(done) break;
    }
    if(done) break;
  }

  rebuild_blocks();
  maybe_need_forwards();
  maybe_need_backwards();
}

void TimelineView::add_pending(const QString &transaction, const matrix::RoomState &state, const matrix::UserID &self, Time time,
                               matrix::EventType type, matrix::event::Content content, std::experimental::optional<matrix::UserID> affected_user) {
  pending_.emplace_back(transaction,
                        EventLike{get_id(), state, self, time, type, content, affected_user});
  rebuild_blocks();
}

void TimelineView::set_at_bottom(bool value) {
  at_bottom_ = value;
}

void TimelineView::resizeEvent(QResizeEvent *) {
  update_layout();
}

void TimelineView::paintEvent(QPaintEvent *) {
  const qreal spacing = block_spacing(*this);
  const qreal half_spacing = std::round(spacing * 0.5);
  const qreal padding = block_padding(*this);
  const auto view = view_rect();

  QPainter painter(viewport());
  painter.fillRect(viewport()->contentsRect(), palette().color(QPalette::Dark));
  painter.setPen(palette().color(QPalette::Text));
  painter.translate(QPointF(0, -view.top()));

  bool animating = false;
  if(view.bottom() > 0 && !at_bottom_) {
    draw_spinner(painter, 0);
    animating = true;
  }

  visible_blocks_.clear();
  bool selecting = false;
  for(auto block = blocks_.rbegin(); block != blocks_.rend(); ++block) {
    const auto &bounds = block->bounds();
    painter.translate(QPointF(0, -std::round(spacing + bounds.height())));
    const auto block_top = painter.worldTransform().dy() + view.top();
    if(block_top > view.bottom()) continue;

    {
      const QRectF outline(0, 0, view.width(), bounds.height() + spacing);
      painter.save();
      painter.setRenderHint(QPainter::Antialiasing);
      QPainterPath path;
      path.addRoundedRect(outline, padding*2, padding*2);
      painter.fillPath(path, palette().color(QPalette::Base));
      painter.restore();
    }

    {
      painter.save();
      painter.translate(QPointF(padding, half_spacing));
      visible_blocks_.emplace_back(*block, QPointF(painter.worldTransform().dx(), painter.worldTransform().dy()));
      selecting = block->draw(painter, selecting, selection_);
      painter.restore();
    }

    if(block_top < view.top()) break;
  }

  const auto top = painter.worldTransform().m32() + view.top();
  if(view.top() < top && !at_top()) {
    draw_spinner(painter, -spinner_space());
    animating = true;
  }

  if(animating) {
    QTimer::singleShot(30, viewport(), static_cast<void (QWidget::*)()>(&QWidget::update));
  }

}

void TimelineView::changeEvent(QEvent *) {
  // Optimization: Block lifecycle could be refactored to construct/polish/flow instead of construct/flow to reduce CPU use
  rebuild_blocks();
}

void TimelineView::mousePressEvent(QMouseEvent *event) {
  dispatch_input(event->localPos(), event);
  if(event->isAccepted()) return;

  if(event->button() == Qt::LeftButton) {
    const bool had_selection = static_cast<bool>(selection_);
    auto now = std::chrono::steady_clock::now();
    if(now - last_click_ <= std::chrono::milliseconds(QGuiApplication::styleHints()->mouseDoubleClickInterval())) {
      click_count_ += 1;
    } else {
      click_count_ = 0;
    }
    constexpr Selection::Mode selection_modes[] = {Selection::Mode::CHARACTER, Selection::Mode::WORD, Selection::Mode::PARAGRAPH};
    selection_.mode = selection_modes[std::min<size_t>(2, click_count_)];
    selection_.begin = *get_cursor(event->localPos(), false);
    selection_.end = selection_.begin;

    if(had_selection)
      viewport()->update();

    QGuiApplication::setOverrideCursor(Qt::IBeamCursor);
    last_click_ = now;
    selection_updating_ = true;
  }
}

void TimelineView::mouseDoubleClickEvent(QMouseEvent *event) {
  mousePressEvent(event);
}

void TimelineView::mouseMoveEvent(QMouseEvent *event) {
  dispatch_input(event->localPos(), event);

  if(!event->isAccepted()) {
    setCursor(Qt::ArrowCursor);
    event->accept();
  }

  if(selection_updating_ && event->buttons() & Qt::LeftButton) {
    auto new_end = *get_cursor(event->localPos(), false);
    if(selection_.end != new_end) {
      selection_.end = new_end;
      viewport()->update();
    }

    QString t = selection_text();
    if(!t.isEmpty())
      QGuiApplication::clipboard()->setText(selection_text(), QClipboard::Selection);
  }
}

void TimelineView::mouseReleaseEvent(QMouseEvent *event) {
  dispatch_input(event->localPos(), event);

  if(event->button() == Qt::LeftButton) {
    QGuiApplication::restoreOverrideCursor();
    selection_updating_ = false;
  }
}

void TimelineView::focusOutEvent(QFocusEvent *e) {
  // Taken from QWidgetTextControl
  if(e->reason() != Qt::ActiveWindowFocusReason
     && e->reason() != Qt::PopupFocusReason) {
    selection_ = Selection{};
    viewport()->update();
  }
}

void TimelineView::contextMenuEvent(QContextMenuEvent *event) {
  dispatch_input(QPointF(event->pos()), event);
}

bool TimelineView::viewportEvent(QEvent *e) {
  if(e->type() == QEvent::ToolTip) {
    auto help = static_cast<QHelpEvent*>(e);
    dispatch_input(QPointF(help->pos()), help);
    if(!help->isAccepted()) {
      QToolTip::hideText();
    }

    return true;
  }
  return QAbstractScrollArea::viewportEvent(e);
}

QString TimelineView::selection_text() const {
  QString result;

  bool selecting = false;
  for(auto block = blocks_.rbegin(); block != blocks_.rend(); ++block) {
    auto r = block->selection_text(selecting, selection_);
    selecting = r.continues;
    if(!r.fragment.isEmpty()) {
      if(result.isEmpty()) {
        result = std::move(r.fragment);
      } else {
        result = std::move(r.fragment) % "\n" % std::move(result);
      }
    }
  }

  return result;
}

void TimelineView::copy() const {
  QString t = selection_text();
  if(!t.isEmpty()) {
    QGuiApplication::clipboard()->setText(t);
    QGuiApplication::clipboard()->setText(t, QClipboard::Selection);
  }
}

QRectF TimelineView::view_rect() const {
  const auto &r = viewport()->contentsRect();
  return r.translated(0, -r.height() - (verticalScrollBar()->maximum() - verticalScrollBar()->value()) + !at_bottom_ * spinner_space());
}

void TimelineView::update_scrollbar(int content_height) {
  auto &scroll = *verticalScrollBar();
  const bool was_at_bottom = scroll.value() == scroll.maximum();
  const auto view_height = viewport()->contentsRect().height();

  content_height += (!at_bottom_ + !at_top()) * spinner_space();

  scroll.setMaximum(content_height > view_height ? content_height - view_height : 0);
  scroll.setPageStep(viewport()->contentsRect().height());
  if(was_at_bottom) scroll.setValue(scroll.maximum());
}

// Whether two events should be assigned to distinct blocks
static bool block_border(const EventLike &a, const EventLike &b) {
  return b.sender != a.sender || !b.time || !a.time || *b.time - *a.time > BLOCK_MERGE_INTERVAL;
}

void TimelineView::rebuild_blocks() {
  // Optimization: defer until visible
  std::deque<EventBlock> new_blocks;
  std::vector<const EventLike *> block_events;
  for(const auto &batch : batches_) {
    for(const auto &event : batch.events) {
      if(!block_events.empty() && block_border(*block_events.back(), event)) {
        new_blocks.emplace_back(*this, thumbnail_cache_, block_events);
        block_events.clear();
      }
      block_events.emplace_back(&event);
    }
  }
  if(at_bottom_) {
    for(const auto &event : pending_) {
      if(!block_events.empty() && block_border(*block_events.back(), event.event)) {
        new_blocks.emplace_back(*this, thumbnail_cache_, block_events);
        block_events.clear();
      }
      block_events.emplace_back(&event.event);
    }
  }
  if(!block_events.empty()) {
    new_blocks.emplace_back(*this, thumbnail_cache_, block_events);
    block_events.clear();
  }
  std::swap(blocks_, new_blocks);
  update_layout();
}

void TimelineView::update_layout() {
  ensurePolished();

  qreal content_height = blocks_.size() * block_spacing(*this);

  const auto width = viewport()->contentsRect().width() - 2*block_padding(*this);
  for(auto &block : blocks_) {
    block.update_layout(width);
    content_height += block.bounds().height();
  }

  update_scrollbar(content_height);

  viewport()->update();
}

void TimelineView::maybe_need_backwards() {
  if(at_top()) return;
  need_backwards();
}

void TimelineView::maybe_need_forwards() {
  if(at_bottom_) return;
  need_forwards();
}

bool TimelineView::at_top() const {
  return !batches_.empty() && batches_.front().events.front().type == matrix::event::room::Create::tag();
}

qreal TimelineView::spinner_space() const {
  return fontMetrics().lineSpacing() * 4;
}

void TimelineView::draw_spinner(QPainter &painter, qreal top) const {
  const qreal extent = spinner_.width() / spinner_.devicePixelRatio();
  painter.save();

  painter.setRenderHint(QPainter::SmoothPixmapTransform);
  painter.translate(view_rect().width() * 0.5, top + spinner_space() * 0.5);
  auto t = std::chrono::time_point_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now());
  constexpr qreal rotation_seconds = 2;
  const qreal angle = 360. * static_cast<qreal>(t.time_since_epoch().count() % static_cast<uint64_t>(1000 * rotation_seconds)) / (1000 * rotation_seconds);
  painter.rotate(angle);
  painter.drawPixmap(QPointF(-extent * 0.5, -extent * 0.5), spinner_);

  painter.restore();
}

void TimelineView::dispatch_input(const QPointF &point, QEvent *input) {
  for(auto &vb : visible_blocks_) {
    const auto rect = vb.bounds();
    if(rect.contains(point)) {
      vb.block().handle_input(point - rect.topLeft(), input);
      return;
    }
  }
  input->ignore();
}

TimelineEventID TimelineView::get_id() { return TimelineEventID{id_counter_++}; }

QRectF TimelineView::VisibleBlock::bounds() const {
  return block_.bounds().translated(origin_);
}

optional<Cursor> TimelineView::get_cursor(const QPointF &point, bool exact) const {
  for(auto vb = visible_blocks_.rbegin(); vb != visible_blocks_.rend(); ++vb) {
    const auto rect = vb->bounds();
    if(point.y() <= rect.bottom()) {
      auto x = vb->block().get_cursor(point - rect.topLeft(), exact);
      if(x) return x->cursor;
      return {};
    }
  }
  if(exact) return {};
  auto x = visible_blocks_.front().block().get_cursor(point - visible_blocks_.front().bounds().topLeft());
  if(x) return x->cursor;
  return {};
}
