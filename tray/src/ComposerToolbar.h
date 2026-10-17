#pragma once

#include <QFrame>
#include <QList>
#include <QPointer>
#include <QPushButton>

class QHBoxLayout;
class QTimer;

// Accent is the green pill of a positive action (Mark as read).
enum class ChipTone { Quiet, Neutral, Success, Warning, Danger, Accent };

// Popup panel anchored above a chip. Escape closes it; closing returns focus to
// the widget that had it at opening: the chip after keyboard activation, the
// message field after a mouse click (chips never take focus on click).
class ChipPopover : public QFrame {
public:
    explicit ChipPopover(QWidget *parent);
    void setContent(QWidget *content);
    QWidget *content() const { return m_content; }
    void setTheme(bool dark);
    void showFor(QWidget *anchor);
    // Keeps an open popover above its anchor after its content changed size.
    void reposition();
protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void hideEvent(QHideEvent *event) override;
private:
    QPointer<QWidget> m_content, m_anchor, m_returnFocus;
};

// A compact notice or status above the message field. A narrow row shows its
// short label (or only its icon); the full label stays its accessible name.
class ToolbarChip : public QPushButton {
    Q_OBJECT
public:
    explicit ToolbarChip(QWidget *parent = nullptr);
    void setLabels(const QString &full, const QString &shortText = {});
    QString fullLabel() const { return m_full; }
    QString shortLabel() const { return m_short; }
    // The complete notice: tooltip and accessible description.
    void setDetail(const QString &detail);
    void setTone(ChipTone tone);
    ChipTone tone() const { return m_tone; }
    void setIconName(const QString &name);
    void setTheme(bool dark);
    // Owners show and hide a chip only here; the toolbar may additionally hide a
    // hideable chip that does not fit.
    void setActive(bool active);
    bool isActive() const { return m_active; }
    void setFitHidden(bool hidden);
    void setCompact(bool compact);
    bool isCompact() const { return m_compact; }
    // The text painted at the current width: the label, elided only when squeezed.
    QString visibleText() const;
    QSize labelSizeHint(bool compact) const;
    QSize sizeHint() const override { return labelSizeHint(m_compact); }
    QSize minimumSizeHint() const override;
    void setPopoverContent(QWidget *content);
    ChipPopover *popover() const { return m_popover; }
    void openPopover();
    void closePopover();
    // A one-second accent border confirming that something was added.
    void flash();
signals:
    // Anything the row width depends on changed.
    void fitChanged();
    // The chip disappeared while it or its popover held keyboard focus.
    void focusReleased();
protected:
    void keyPressEvent(QKeyEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
private:
    QString labelText(bool compact) const;
    void render();
    void updateVisibility();
    QString m_full, m_short, m_detail, m_icon, m_renderedIcon;
    ChipTone m_tone = ChipTone::Neutral;
    bool m_dark = true, m_active = false, m_fitHidden = false, m_compact = false, m_flashing = false;
    ChipPopover *m_popover = nullptr;
    QTimer *m_flashTimer;
};

// One fixed-height row above the message field. The slot table fixes each item's
// zone, order and priority; a narrow row shortens low-priority chips first, then
// hides Mark as read, and only then elides the widest chips. It never wraps.
class ComposerToolbar : public QWidget {
    Q_OBJECT
public:
    enum class Slot { Attachments, Voice, VoiceMode, VoiceCancel, VoiceResult, Latest, MarkRead, Compaction, CompactionCancel, UsageLimit, Recovery, Cache, Context };
    static constexpr int RowHeight = 34;
    explicit ComposerToolbar(QWidget *parent = nullptr);
    void add(Slot slot, QWidget *item);
    void setTheme(bool dark);
    void closePopovers();
    // Takes keyboard focus when a chip that held it disappears.
    void setFocusFallback(QWidget *widget) { m_focusFallback = widget; }
    // Fits the row now; other changes refit on the next event-loop pass.
    void fit();
protected:
    bool event(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
private:
    struct Item { Slot slot; QPointer<QWidget> widget; };
    static bool leading(Slot slot);
    static int priority(Slot slot);
    void scheduleFit();
    QList<int> fitInputs() const;
    int requiredWidth() const;
    QList<Item> m_items;
    QHBoxLayout *m_leading, *m_trailing;
    QList<int> m_fitted;
    QPointer<QWidget> m_focusFallback;
    bool m_dark = true, m_fitting = false, m_scheduled = false;
};
