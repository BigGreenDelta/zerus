#pragma once
#include <QDate>
#include <QPushButton>

class QDateEdit;
class QFrame;
class QLabel;

// The open panel edits a draft. Only a preset or Apply changes the active query.
class ArchiveDatePicker : public QPushButton {
    Q_OBJECT
public:
    explicit ArchiveDatePicker(QWidget *parent = nullptr);
    void setTheme(bool dark);
    QPair<qint64, qint64> range() const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
signals:
    void rangeChanged();
protected:
    void paintEvent(QPaintEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool eventFilter(QObject *, QEvent *) override;
private:
    void openPanel();
    void apply(int period);
    void updateDraft();
    void updateCaption();
    int m_period = 0;
    QDate m_first = QDate::currentDate().addDays(-29), m_last = QDate::currentDate();
    bool m_dark = true;
    QFrame *m_panel;
    QDateEdit *m_from, *m_until;
    QLabel *m_hint;
    QPushButton *m_apply;
};
