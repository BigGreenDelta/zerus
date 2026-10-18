#include "TerminalScreen.h"

#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QScrollBar>
#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr int Padding = 8;
constexpr int MaxHistoryLines = 5000;
constexpr qsizetype MaxHistoryBytes = 24 * 1024 * 1024;
const QColor Background(16, 21, 27), Foreground(216, 226, 237),
    Selection(53, 82, 103);
bool before(QPoint a, QPoint b) {
  return a.y() < b.y() || (a.y() == b.y() && a.x() < b.x());
}
bool copyKey(QKeyEvent *event) {
#ifdef Q_OS_MACOS
  return event->modifiers() == Qt::ControlModifier && event->key() == Qt::Key_C;
#else
  return event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier) &&
         event->key() == Qt::Key_C;
#endif
}
bool pasteKey(QKeyEvent *event) {
#ifdef Q_OS_MACOS
  if (event->modifiers() == Qt::ControlModifier && event->key() == Qt::Key_V)
    return true;
#else
  if (event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier) &&
      event->key() == Qt::Key_V)
    return true;
#endif
  return event->modifiers() == Qt::ShiftModifier &&
         event->key() == Qt::Key_Insert;
}
} // namespace

TerminalScreen::TerminalScreen(QWidget *parent) : QAbstractScrollArea(parent) {
  setObjectName(QStringLiteral("terminalScreen"));
  setFocusPolicy(Qt::StrongFocus);
  setAttribute(Qt::WA_InputMethodEnabled);
  setFrameShape(QFrame::NoFrame);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
  viewport()->setCursor(Qt::IBeamCursor);
  viewport()->setMouseTracking(true);
  viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
  QFont terminalFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  terminalFont.setPointSizeF(11.0);
  terminalFont.setStyleHint(QFont::TypeWriter);
  setFont(terminalFont);
  refreshMetrics();
  m_terminal = vterm_new(m_rows, m_columns);
  vterm_set_utf8(m_terminal, 1);
  vterm_output_set_callback(m_terminal, &TerminalScreen::produced, this);
  m_screen = vterm_obtain_screen(m_terminal);
  m_state = vterm_obtain_state(m_terminal);
  static const VTermScreenCallbacks callbacks{
      damaged, moved,    cursorMoved, propertyChanged, ring,
      resized, pushLine, popLine,     clearHistory};
  vterm_screen_set_callbacks(m_screen, &callbacks, this);
  vterm_screen_enable_altscreen(m_screen, 1);
  vterm_screen_enable_reflow(m_screen, true);
  vterm_screen_set_damage_merge(m_screen, VTERM_DAMAGE_SCROLL);
  VTermColor fg, bg;
  vterm_color_rgb(&fg, Foreground.red(), Foreground.green(), Foreground.blue());
  vterm_color_rgb(&bg, Background.red(), Background.green(), Background.blue());
  vterm_screen_set_default_colors(m_screen, &fg, &bg);
  reset();
  connect(verticalScrollBar(), &QScrollBar::valueChanged, viewport(),
          qOverload<>(&QWidget::update));
  m_blink.setInterval(500);
  connect(&m_blink, &QTimer::timeout, this, [this] {
    m_blinkOn = !m_blinkOn;
    if (hasFocus() && m_cursorBlink && m_cursorVisible)
      updateCursor();
  });
  m_blink.start();
}
TerminalScreen::~TerminalScreen() { vterm_free(m_terminal); }
void TerminalScreen::reset() {
  m_history.clear();
  m_historyBytes = 0;
  m_selectionStart = m_selectionEnd = QPoint(-1, -1);
  m_preedit.clear();
  m_title.clear();
  vterm_screen_reset(m_screen, 1);
  vterm_screen_flush_damage(m_screen);
  updateScrollBar(true);
  viewport()->update();
}
void TerminalScreen::feed(const QByteArray &bytes) {
  vterm_input_write(m_terminal, bytes.constData(),
                    static_cast<size_t>(bytes.size()));
  vterm_screen_flush_damage(m_screen);
}
void TerminalScreen::produced(const char *bytes, size_t size, void *user) {
  auto *self = static_cast<TerminalScreen *>(user);
  if (self->m_batchedOutput)
    self->m_batchedOutput->append(bytes, static_cast<qsizetype>(size));
  else
    emit self->output(QByteArray(bytes, static_cast<qsizetype>(size)));
}
void TerminalScreen::updateCells(VTermRect cells) {
  // libvterm reports coordinates in the live screen; the viewport may show
  // scrollback above it. Include neighbours for a double-width glyph's tail.
  const int offset = m_alternate ? 0 :
      static_cast<int>(m_history.size()) - verticalScrollBar()->value();
  const int left = std::max(0, cells.start_col - 1);
  const int right = std::min(m_columns, cells.end_col + 1);
  const int top = std::max(0, cells.start_row + offset);
  const int bottom = std::min(m_rows, cells.end_row + offset);
  if (right > left && bottom > top)
    viewport()->update(QRect(Padding + left * m_cellWidth,
                             Padding + top * m_cellHeight,
                             (right - left) * m_cellWidth,
                             (bottom - top) * m_cellHeight));
}
void TerminalScreen::updateCursor() {
  if (verticalScrollBar()->value() != verticalScrollBar()->maximum())
    return;
  updateCells({m_cursor.row, m_cursor.row + 1, m_cursor.col,
               m_preedit.isEmpty() ? m_cursor.col + 1 : m_columns});
}
int TerminalScreen::damaged(VTermRect cells, void *user) {
  static_cast<TerminalScreen *>(user)->updateCells(cells);
  return 1;
}
int TerminalScreen::moved(VTermRect dest, VTermRect src, void *user) {
  damaged(src, user);
  return damaged(dest, user);
}
int TerminalScreen::cursorMoved(VTermPos pos, VTermPos, int visible,
                                void *user) {
  auto *self = static_cast<TerminalScreen *>(user);
  self->updateCursor();
  self->m_cursor = pos;
  self->m_cursorVisible = visible;
  self->m_blinkOn = true;
  self->updateCursor();
  return 1;
}
int TerminalScreen::propertyChanged(VTermProp prop, VTermValue *value,
                                    void *user) {
  auto *self = static_cast<TerminalScreen *>(user);
  switch (prop) {
  case VTERM_PROP_CURSORVISIBLE:
    self->m_cursorVisible = value->boolean;
    self->updateCursor();
    return 1;
  case VTERM_PROP_CURSORBLINK:
    self->m_cursorBlink = value->boolean;
    self->updateCursor();
    return 1;
  case VTERM_PROP_CURSORSHAPE:
    self->m_cursorShape = value->number;
    self->updateCursor();
    return 1;
  case VTERM_PROP_MOUSE:
    self->m_mouseMode = value->number;
    return 1;
  case VTERM_PROP_REVERSE:
    self->m_reverse = value->boolean;
    break;
  case VTERM_PROP_ALTSCREEN:
    self->m_alternate = value->boolean;
    self->m_selectionStart = self->m_selectionEnd = QPoint(-1, -1);
    self->updateScrollBar(true);
    break;
  case VTERM_PROP_TITLE:
    if (value->string.initial)
      self->m_title.clear();
    if (self->m_title.size() < 4096)
      self->m_title +=
          QString::fromUtf8(value->string.str,
                            static_cast<qsizetype>(value->string.len))
              .left(4096 - self->m_title.size());
    if (value->string.final)
      emit self->titleChanged(self->m_title);
    return 1;
  default:
    return 1;
  }
  self->viewport()->update();
  return 1;
}
int TerminalScreen::ring(void *user) {
  emit static_cast<TerminalScreen *>(user)->bell();
  return 1;
}
int TerminalScreen::resized(int rows, int columns, void *user) {
  auto *self = static_cast<TerminalScreen *>(user);
  self->m_rows = rows;
  self->m_columns = columns;
  self->updateScrollBar(true);
  return 1;
}
int TerminalScreen::pushLine(int columns, const VTermScreenCell *cells,
                             void *user) {
  auto *self = static_cast<TerminalScreen *>(user);
  const bool bottom = self->verticalScrollBar()->value() ==
                      self->verticalScrollBar()->maximum();
  self->m_history.emplace_back(cells, cells + columns);
  self->m_historyBytes += columns * sizeof(VTermScreenCell);
  while (self->m_history.size() > MaxHistoryLines ||
         self->m_historyBytes > MaxHistoryBytes) {
    self->m_historyBytes -=
        self->m_history.front().size() * sizeof(VTermScreenCell);
    self->m_history.pop_front();
    self->m_selectionStart.ry()--;
    self->m_selectionEnd.ry()--;
    if (!bottom) {
      self->verticalScrollBar()->setValue(self->verticalScrollBar()->value() -
                                          1);
      self->viewport()->update();
    }
  }
  self->updateScrollBar(bottom);
  return 1;
}
int TerminalScreen::popLine(int columns, VTermScreenCell *cells, void *user) {
  auto *self = static_cast<TerminalScreen *>(user);
  if (self->m_history.empty())
    return 0;
  const auto &line = self->m_history.back();
  std::fill(cells, cells + columns, VTermScreenCell{});
  std::copy_n(line.begin(), std::min(columns, static_cast<int>(line.size())),
              cells);
  self->m_historyBytes -= line.size() * sizeof(VTermScreenCell);
  self->m_history.pop_back();
  self->updateScrollBar(true);
  return 1;
}
int TerminalScreen::clearHistory(void *user) {
  auto *self = static_cast<TerminalScreen *>(user);
  self->m_history.clear();
  self->m_historyBytes = 0;
  self->m_selectionStart = self->m_selectionEnd = QPoint(-1, -1);
  self->updateScrollBar(true);
  self->viewport()->update();
  return 1;
}
void TerminalScreen::updateScrollBar(bool wasAtBottom) {
  verticalScrollBar()->setPageStep(m_rows);
  verticalScrollBar()->setRange(
      0, m_alternate ? 0 : static_cast<int>(m_history.size()));
  if (wasAtBottom)
    scrollToBottom();
}
void TerminalScreen::scrollToBottom() {
  verticalScrollBar()->setValue(verticalScrollBar()->maximum());
}
void TerminalScreen::refreshMetrics() {
  // Use the effective widget font and its actual paint device. Parent style
  // sheets and moving between screens can change both after construction.
  const QFontMetricsF metrics(font(), viewport());
  m_cellWidth = std::max(1, static_cast<int>(std::ceil(
                                metrics.horizontalAdvance(QLatin1Char('M')))));
  m_cellHeight = std::max(1, static_cast<int>(std::ceil(metrics.height())) + 2);
  m_ascent = static_cast<int>(std::ceil(metrics.ascent())) + 1;
  // setFont() sends FontChange synchronously inside our constructor, before
  // libvterm is initialized. Resizing later reflows, never resets, its buffer.
  if (m_terminal && m_screen) {
    updateGrid();
    viewport()->update();
  }
}
void TerminalScreen::updateGrid() {
  const QSize cells(
      std::clamp((viewport()->width() - 2 * Padding) / m_cellWidth, 2, 1000),
      std::clamp((viewport()->height() - 2 * Padding) / m_cellHeight, 2, 500));
  if (cells == terminalSize())
    return;
  m_selectionStart = m_selectionEnd = QPoint(-1, -1);
  vterm_set_size(m_terminal, cells.height(), cells.width());
  vterm_screen_flush_damage(m_screen);
  emit sizeChanged(cells);
}
void TerminalScreen::resizeEvent(QResizeEvent *event) {
  QAbstractScrollArea::resizeEvent(event);
  updateGrid();
}
VTermScreenCell TerminalScreen::screenCell(int row, int column) const {
  VTermScreenCell result{};
  if (row >= 0 && row < m_rows && column >= 0 && column < m_columns)
    vterm_screen_get_cell(m_screen, VTermPos{row, column}, &result);
  return result;
}
VTermScreenCell TerminalScreen::cell(int row, int column) const {
  if (m_alternate)
    return screenCell(row, column);
  if (row >= 0 && row < static_cast<int>(m_history.size())) {
    const auto &line = m_history[static_cast<size_t>(row)];
    return column >= 0 && column < line.size() ? line[column]
                                               : VTermScreenCell{};
  }
  return screenCell(row - static_cast<int>(m_history.size()), column);
}
QString TerminalScreen::characters(const VTermScreenCell &value) {
  if (value.chars[0] == std::numeric_limits<uint32_t>::max())
    return {};
  if (value.chars[0] == 0)
    return QStringLiteral(" ");
  int count = 0;
  while (count < VTERM_MAX_CHARS_PER_CELL && value.chars[count])
    ++count;
  return QString::fromUcs4(reinterpret_cast<const char32_t *>(value.chars),
                           count);
}
QString TerminalScreen::screenText() const {
  QStringList rows;
  for (int row = 0; row < m_rows; ++row) {
    QString text;
    for (int column = 0; column < m_columns; ++column)
      text += characters(screenCell(row, column));
    while (text.endsWith(QLatin1Char(' ')))
      text.chop(1);
    rows.append(text);
  }
  return rows.join(QLatin1Char('\n'));
}
QColor TerminalScreen::color(VTermColor value) const {
  vterm_screen_convert_color_to_rgb(m_screen, &value);
  return QColor(value.rgb.red, value.rgb.green, value.rgb.blue);
}
bool TerminalScreen::isSelected(int row, int column) const {
  if (m_selectionStart.y() < 0 || m_selectionEnd.y() < 0)
    return false;
  auto start = m_selectionStart, end = m_selectionEnd;
  if (before(end, start))
    std::swap(start, end);
  const QPoint current(column, row);
  return !before(current, start) && !before(end, current);
}
void TerminalScreen::paintEvent(QPaintEvent *event) {
  QPainter painter(viewport());
  painter.fillRect(event->rect(), Background);
  const int offset = verticalScrollBar()->value();
  // Qt clips paint output, but it does not skip our text layout work. Restrict
  // the cell loop too, including for separate old/new cursor rectangles.
  for (const QRect &dirty : event->region()) {
    const int firstRow = std::clamp((dirty.top() - Padding) / m_cellHeight, 0, m_rows);
    const int lastRow = std::clamp((dirty.bottom() - Padding) / m_cellHeight + 1, 0, m_rows);
    const int firstColumn = std::clamp((dirty.left() - Padding) / m_cellWidth - 1, 0, m_columns);
    const int lastColumn = std::clamp((dirty.right() - Padding) / m_cellWidth + 1, 0, m_columns);
    for (int row = firstRow; row < lastRow; ++row) {
      for (int column = firstColumn; column < lastColumn; ++column) {
        const auto value = cell(offset + row, column);
        if (value.chars[0] == std::numeric_limits<uint32_t>::max())
          continue;
        QColor fg = color(value.fg), bg = color(value.bg);
        if (bool(value.attrs.reverse) != m_reverse)
          std::swap(fg, bg);
        if (isSelected(offset + row, column))
          bg = Selection;
        const QRect rect(
            Padding + column * m_cellWidth, Padding + row * m_cellHeight,
            std::max(1, int(value.width)) * m_cellWidth, m_cellHeight);
        painter.fillRect(rect, bg);
        QFont glyphFont = font();
        glyphFont.setBold(value.attrs.bold);
        glyphFont.setItalic(value.attrs.italic);
        glyphFont.setUnderline(value.attrs.underline);
        glyphFont.setStrikeOut(value.attrs.strike);
        painter.setFont(glyphFont);
        painter.setPen(fg);
        const bool blank = !value.chars[0] ||
            (value.chars[0] == ' ' && !value.chars[1]);
        if (!value.attrs.conceal &&
            (!blank || value.attrs.underline || value.attrs.strike)) {
          painter.save();
          painter.setClipRect(rect);
          painter.drawText(rect.x(), rect.y() + m_ascent, characters(value));
          painter.restore();
        }
      }
    }
  }
  const bool bottom = offset == verticalScrollBar()->maximum();
  if (bottom && m_cursorVisible &&
      (!hasFocus() || !m_cursorBlink || m_blinkOn)) {
    QRect rect(Padding + m_cursor.col * m_cellWidth,
               Padding + m_cursor.row * m_cellHeight, m_cellWidth,
               m_cellHeight);
    painter.setPen(Foreground);
    if (!hasFocus()) {
      // A stroked outline is centred on the cell edge: above a device pixel
      // ratio of 1 its top half lands in the row above, which updateCursor()
      // never invalidates. Fill the four edges inside the cell instead.
      painter.fillRect(QRect(rect.topLeft(), QSize(rect.width(), 1)), Foreground);
      painter.fillRect(QRect(rect.left(), rect.bottom(), rect.width(), 1), Foreground);
      painter.fillRect(QRect(rect.topLeft(), QSize(1, rect.height())), Foreground);
      painter.fillRect(QRect(rect.right(), rect.top(), 1, rect.height()), Foreground);
    }
    else if (m_cursorShape == VTERM_PROP_CURSORSHAPE_BAR_LEFT)
      painter.fillRect(QRect(rect.topLeft(), QSize(2, rect.height())),
                       Foreground);
    else if (m_cursorShape == VTERM_PROP_CURSORSHAPE_UNDERLINE)
      painter.fillRect(
          QRect(rect.bottomLeft() - QPoint(0, 1), QSize(rect.width(), 2)),
          Foreground);
    else
      painter.fillRect(rect, QColor(216, 226, 237, 95));
  }
  if (bottom && !m_preedit.isEmpty()) {
    painter.setFont(font());
    painter.setPen(Foreground);
    painter.drawText(Padding + m_cursor.col * m_cellWidth,
                     Padding + m_cursor.row * m_cellHeight + m_ascent,
                     m_preedit);
  }
}
VTermModifier TerminalScreen::modifiers(Qt::KeyboardModifiers mods) {
  int result = VTERM_MOD_NONE;
  if (mods.testFlag(Qt::ShiftModifier))
    result |= VTERM_MOD_SHIFT;
  if (mods.testFlag(Qt::AltModifier))
    result |= VTERM_MOD_ALT;
#ifdef Q_OS_MACOS
  if (mods.testFlag(Qt::MetaModifier))
    result |= VTERM_MOD_CTRL;
#else
  if (mods.testFlag(Qt::ControlModifier))
    result |= VTERM_MOD_CTRL;
#endif
  return static_cast<VTermModifier>(result);
}
bool TerminalScreen::event(QEvent *event) {
  if (event->type() == QEvent::FontChange ||
      event->type() == QEvent::ApplicationFontChange ||
      event->type() == QEvent::StyleChange ||
      event->type() == QEvent::ScreenChangeInternal
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
      || event->type() == QEvent::DevicePixelRatioChange
#endif
  ) {
    const bool handled = QAbstractScrollArea::event(event);
    refreshMetrics();
    return handled;
  }
  // QWidget normally consumes Tab for focus traversal before keyPressEvent.
  // An interactive terminal must pass Tab and Shift-Tab to its application.
  if (event->type() == QEvent::KeyPress) {
    auto *key = static_cast<QKeyEvent *>(event);
    if (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab) {
      keyPressEvent(key);
      return true;
    }
  }
  if (event->type() == QEvent::ShortcutOverride) {
    auto *key = static_cast<QKeyEvent *>(event);
#ifdef Q_OS_MACOS
    // Qt maps Command to ControlModifier and the physical Control key to
    // MetaModifier. Keep application Command shortcuts except copy/paste.
    if (key->modifiers().testFlag(Qt::ControlModifier) && !copyKey(key) &&
        !pasteKey(key))
      return QAbstractScrollArea::event(event);
#endif
    key->accept();
    return true;
  }
  return QAbstractScrollArea::event(event);
}
void TerminalScreen::keyPressEvent(QKeyEvent *event) {
  if (copyKey(event)) {
    copySelection();
    return;
  }
  if (pasteKey(event)) {
    pasteClipboard();
    return;
  }
  if (event->modifiers() == Qt::ShiftModifier &&
      (event->key() == Qt::Key_PageUp || event->key() == Qt::Key_PageDown)) {
    verticalScrollBar()->setValue(
        verticalScrollBar()->value() +
        (event->key() == Qt::Key_PageUp ? -m_rows : m_rows));
    return;
  }
  scrollToBottom();
  m_blinkOn = true;
  VTermKey key = VTERM_KEY_NONE;
  switch (event->key()) {
  case Qt::Key_Return:
  case Qt::Key_Enter:
    key = VTERM_KEY_ENTER;
    break;
  case Qt::Key_Tab:
  case Qt::Key_Backtab:
    key = VTERM_KEY_TAB;
    break;
  case Qt::Key_Backspace:
    key = VTERM_KEY_BACKSPACE;
    break;
  case Qt::Key_Escape:
    key = VTERM_KEY_ESCAPE;
    break;
  case Qt::Key_Up:
    key = VTERM_KEY_UP;
    break;
  case Qt::Key_Down:
    key = VTERM_KEY_DOWN;
    break;
  case Qt::Key_Left:
    key = VTERM_KEY_LEFT;
    break;
  case Qt::Key_Right:
    key = VTERM_KEY_RIGHT;
    break;
  case Qt::Key_Insert:
    key = VTERM_KEY_INS;
    break;
  case Qt::Key_Delete:
    key = VTERM_KEY_DEL;
    break;
  case Qt::Key_Home:
    key = VTERM_KEY_HOME;
    break;
  case Qt::Key_End:
    key = VTERM_KEY_END;
    break;
  case Qt::Key_PageUp:
    key = VTERM_KEY_PAGEUP;
    break;
  case Qt::Key_PageDown:
    key = VTERM_KEY_PAGEDOWN;
    break;
  default:
    if (event->key() >= Qt::Key_F1 && event->key() <= Qt::Key_F35)
      key = static_cast<VTermKey>(
          VTERM_KEY_FUNCTION(event->key() - Qt::Key_F1 + 1));
    break;
  }
  auto mods = modifiers(event->modifiers());
  if (event->key() == Qt::Key_Backtab)
    mods = static_cast<VTermModifier>(mods | VTERM_MOD_SHIFT);
  if (key != VTERM_KEY_NONE)
    vterm_keyboard_key(m_terminal, key, mods);
  else if ((mods == VTERM_MOD_CTRL ||
            mods == (VTERM_MOD_CTRL | VTERM_MOD_ALT)) &&
           (event->key() == Qt::Key_I || event->key() == Qt::Key_J ||
            event->key() == Qt::Key_M || event->key() == Qt::Key_BracketLeft)) {
    // Preserve conventional C0 aliases without enabling an extended
    // keyboard protocol which the attached application has not requested.
    vterm_keyboard_unichar(m_terminal, event->key() & 0x1f,
                           static_cast<VTermModifier>(mods & VTERM_MOD_ALT));
  } else if ((mods & VTERM_MOD_CTRL) && event->key() >= Qt::Key_A &&
             event->key() <= Qt::Key_Z)
    vterm_keyboard_unichar(m_terminal, 'a' + event->key() - Qt::Key_A, mods);
  else if ((mods & VTERM_MOD_CTRL) && event->key() >= Qt::Key_Space &&
           event->key() <= Qt::Key_AsciiTilde)
    vterm_keyboard_unichar(m_terminal, event->key(), mods);
  else if (!event->text().isEmpty()) {
    // Shift is already represented in the Unicode text from the layout.
    mods = static_cast<VTermModifier>(mods & ~VTERM_MOD_SHIFT);
    for (char32_t ch : event->text().toUcs4())
      vterm_keyboard_unichar(m_terminal, ch, mods);
  }
  event->accept();
  updateCursor();
}
void TerminalScreen::pasteText(const QString &text) {
  if (text.isEmpty())
    return;
  scrollToBottom();
  QString normalized;
  normalized.reserve(text.size());
  for (QChar ch : text) {
    // Clipboard text is text, not terminal input commands. In particular an
    // embedded ESC [ 201 ~ must never break out of bracketed paste framing.
    const auto code = ch.unicode();
    if ((code >= 0x20 && code != 0x7f && !(code >= 0x80 && code <= 0x9f)) ||
        code == '\t' || code == '\n' || code == '\r')
      normalized.append(ch);
  }
  if (normalized.isEmpty())
    return;
  normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
  normalized.replace(QLatin1Char('\n'), QLatin1Char('\r'));
  QByteArray framed;
  m_batchedOutput = &framed;
  vterm_keyboard_start_paste(m_terminal);
  framed.append(normalized.toUtf8());
  vterm_keyboard_end_paste(m_terminal);
  m_batchedOutput = nullptr;
  // The PTY queue accepts/rejects the complete paste atomically, including its
  // end marker. A full queue cannot leave the target stuck in paste mode.
  emit output(framed);
}
QPoint TerminalScreen::position(const QPointF &point) const {
  return QPoint(
      std::clamp((int(point.x()) - Padding) / m_cellWidth, 0, m_columns - 1),
      verticalScrollBar()->value() +
          std::clamp((int(point.y()) - Padding) / m_cellHeight, 0, m_rows - 1));
}
void TerminalScreen::mouseInput(QMouseEvent *event, bool pressed) {
  const QPoint pos = position(event->position());
  vterm_mouse_move(m_terminal, pos.y() - verticalScrollBar()->value(), pos.x(),
                   modifiers(event->modifiers()));
  const int button = event->button() == Qt::LeftButton     ? 1
                     : event->button() == Qt::MiddleButton ? 2
                     : event->button() == Qt::RightButton  ? 3
                                                           : 0;
  if (button)
    vterm_mouse_button(m_terminal, button, pressed,
                       modifiers(event->modifiers()));
}
void TerminalScreen::mousePressEvent(QMouseEvent *event) {
  setFocus(Qt::MouseFocusReason);
  if (m_mouseMode != VTERM_PROP_MOUSE_NONE &&
      !event->modifiers().testFlag(Qt::ShiftModifier)) {
    mouseInput(event, true);
    return;
  }
  if (event->button() == Qt::LeftButton) {
    m_selectionStart = m_selectionEnd = position(event->position());
    m_selecting = true;
    viewport()->update();
  }
}
void TerminalScreen::mouseMoveEvent(QMouseEvent *event) {
  if (m_selecting) {
    m_selectionEnd = position(event->position());
    viewport()->update();
  } else if (m_mouseMode != VTERM_PROP_MOUSE_NONE &&
             !event->modifiers().testFlag(Qt::ShiftModifier))
    mouseInput(event, false);
}
void TerminalScreen::mouseReleaseEvent(QMouseEvent *event) {
  if (m_selecting && event->button() == Qt::LeftButton) {
    m_selectionEnd = position(event->position());
    m_selecting = false;
    viewport()->update();
  } else if (m_mouseMode != VTERM_PROP_MOUSE_NONE &&
             !event->modifiers().testFlag(Qt::ShiftModifier))
    mouseInput(event, false);
}
void TerminalScreen::mouseDoubleClickEvent(QMouseEvent *event) {
  if (m_mouseMode != VTERM_PROP_MOUSE_NONE &&
      !event->modifiers().testFlag(Qt::ShiftModifier)) {
    mouseInput(event, true);
    return;
  }
  if (event->button() != Qt::LeftButton)
    return;
  auto first = position(event->position()), last = first;
  auto word = [this](QPoint pos) {
    const QString text = characters(cell(pos.y(), pos.x()));
    return !text.trimmed().isEmpty();
  };
  while (first.x() > 0 && word(first - QPoint(1, 0)))
    first.rx()--;
  while (last.x() < m_columns - 1 && word(last + QPoint(1, 0)))
    last.rx()++;
  m_selectionStart = first;
  m_selectionEnd = last;
  m_selecting = false;
  viewport()->update();
}
QString TerminalScreen::selectedText() const {
  auto first = m_selectionStart, last = m_selectionEnd;
  if (first.y() < 0 || last.y() < 0)
    return {};
  if (before(last, first))
    std::swap(first, last);
  QStringList lines;
  for (int row = first.y(); row <= last.y(); ++row) {
    QString text;
    const int begin = row == first.y() ? first.x() : 0;
    const int end = row == last.y() ? last.x() : m_columns - 1;
    for (int column = begin; column <= end; ++column)
      text += characters(cell(row, column));
    while (text.endsWith(QLatin1Char(' ')))
      text.chop(1);
    lines.append(text);
  }
  return lines.join(QLatin1Char('\n'));
}
void TerminalScreen::copySelection() {
  const auto text = selectedText();
  if (!text.isEmpty())
    QApplication::clipboard()->setText(text);
}
void TerminalScreen::pasteClipboard() {
  pasteText(QApplication::clipboard()->text());
}
void TerminalScreen::contextMenuEvent(QContextMenuEvent *event) {
  if (m_mouseMode != VTERM_PROP_MOUSE_NONE &&
      !QApplication::keyboardModifiers().testFlag(Qt::ShiftModifier))
    return;
  QMenu menu(this);
  auto *copy = menu.addAction(tr("Copy selection"), this,
                              &TerminalScreen::copySelection);
  copy->setEnabled(!selectedText().isEmpty());
  menu.addAction(tr("Paste"), this, &TerminalScreen::pasteClipboard);
  menu.exec(event->globalPos());
}
void TerminalScreen::wheelEvent(QWheelEvent *event) {
  m_wheelDelta += event->angleDelta().y() != 0 ? event->angleDelta().y()
                                               : event->pixelDelta().y() * 3;
  const int steps = m_wheelDelta / 120;
  m_wheelDelta %= 120;
  if (m_mouseMode != VTERM_PROP_MOUSE_NONE &&
      !event->modifiers().testFlag(Qt::ShiftModifier)) {
    const auto pos = position(event->position());
    const auto mods = modifiers(event->modifiers());
    vterm_mouse_move(m_terminal, pos.y() - verticalScrollBar()->value(),
                     pos.x(), mods);
    for (int n = 0; n < std::min(100, std::abs(steps)); ++n) {
      vterm_mouse_button(m_terminal, steps > 0 ? 4 : 5, true, mods);
      vterm_mouse_button(m_terminal, steps > 0 ? 4 : 5, false, mods);
    }
  } else
    verticalScrollBar()->setValue(verticalScrollBar()->value() - steps * 3);
  event->accept();
}
void TerminalScreen::focusInEvent(QFocusEvent *event) {
  QAbstractScrollArea::focusInEvent(event);
  m_blinkOn = true;
  vterm_state_focus_in(m_state);
  updateCursor();
}
void TerminalScreen::focusOutEvent(QFocusEvent *event) {
  vterm_state_focus_out(m_state);
  QAbstractScrollArea::focusOutEvent(event);
  updateCursor();
}
void TerminalScreen::inputMethodEvent(QInputMethodEvent *event) {
  for (char32_t ch : event->commitString().toUcs4())
    vterm_keyboard_unichar(m_terminal, ch, VTERM_MOD_NONE);
  updateCursor();
  m_preedit = event->preeditString();
  event->accept();
  updateCursor();
}
QVariant TerminalScreen::inputMethodQuery(Qt::InputMethodQuery query) const {
  if (query == Qt::ImEnabled)
    return true;
  if (query == Qt::ImFont)
    return font();
  if (query == Qt::ImCursorRectangle)
    return QRect(Padding + m_cursor.col * m_cellWidth,
                 Padding + m_cursor.row * m_cellHeight, m_cellWidth,
                 m_cellHeight);
  return QAbstractScrollArea::inputMethodQuery(query);
}
