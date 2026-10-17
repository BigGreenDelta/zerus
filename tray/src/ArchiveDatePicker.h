#pragma once
#include <QDate>
#include <QPushButton>

class QDateEdit;
class QFrame;
class QLabel;

// Presets and calendar edits change only the draft. Apply commits the query.
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
    void stage(int period);
    void editDraft(QDateEdit *edited);
    void apply();
    void updateDraft();
    void updateCaption();
    int m_period = 0;
    int m_draftPeriod = 0;
    QDate m_first = QDate::currentDate().addDays(-29), m_last = QDate::currentDate();
    bool m_dark = true;
    QFrame *m_panel;
    QDateEdit *m_from, *m_until;
    QLabel *m_hint;
    QPushButton *m_apply;
};
