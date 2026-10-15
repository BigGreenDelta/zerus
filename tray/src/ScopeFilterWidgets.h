#pragma once
#include <QCheckBox>
#include <QLayout>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QStyleOptionButton>

namespace ScopeFilterWidgets {
class Choice final : public QCheckBox {
public:
    Choice(const QString &text, bool dark) : QCheckBox(text), m_ink(dark ? "#10231b" : "#ffffff") {
        setAttribute(Qt::WA_LayoutUsesWidgetRect);
        setMinimumHeight(32);
        // QMacStyle's native indicator can paint outside its supplied rectangle.
        setStyleSheet(QString("QCheckBox { padding:2px 6px; spacing:8px; } QCheckBox::indicator { width:14px; height:14px; border:1px solid %1; border-radius:4px; background:%2; } QCheckBox::indicator:checked { background:%3; border-color:%3; } QCheckBox::indicator:hover, QCheckBox::indicator:focus { border-color:%3; }")
            .arg(dark ? "#a2adbc" : "#627082", dark ? "#1c2229" : "#ffffff", dark ? "#8bdfc0" : "#167357"));
    }
protected:
    void paintEvent(QPaintEvent *event) override {
        QCheckBox::paintEvent(event);
        if (!isChecked()) return;
        QStyleOptionButton option; initStyleOption(&option);
        const QRectF indicator = style()->subElementRect(QStyle::SE_CheckBoxIndicator, &option, this);
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(m_ink, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath tick; tick.moveTo(indicator.left() + indicator.width()*.25, indicator.top() + indicator.height()*.52);
        tick.lineTo(indicator.left() + indicator.width()*.44, indicator.top() + indicator.height()*.70);
        tick.lineTo(indicator.left() + indicator.width()*.77, indicator.top() + indicator.height()*.31);
        painter.drawPath(tick);
    }
private:
    QColor m_ink;
};
class ChipFlow final : public QLayout {
public:
    explicit ChipFlow(QWidget *parent) : QLayout(parent) { setContentsMargins(0,0,0,0); setSpacing(5); }
    ~ChipFlow() override { while (auto *item = takeAt(0)) delete item; }
    void addItem(QLayoutItem *item) override { m_items.append(item); }
    int count() const override { return int(m_items.size()); }
    QLayoutItem *itemAt(int i) const override { return m_items.value(i); }
    QLayoutItem *takeAt(int i) override { return i >= 0 && i < m_items.size() ? m_items.takeAt(i) : nullptr; }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(QRect(0,0,width,0), false); }
    QSize sizeHint() const override { return {200, heightForWidth(parentWidget()->width())}; }
    QSize minimumSize() const override { return {0, m_items.isEmpty() ? 0 : 27}; }
    void setGeometry(const QRect &r) override { QLayout::setGeometry(r); arrange(r,true); }
private:
    int arrange(const QRect &r, bool apply) const {
        int x = r.x(), y = r.y(), height = 0;
        for (auto *item : m_items) {
            const QSize s(qMin(r.width(),item->sizeHint().width()),item->sizeHint().height());
            if (x > r.x() && x + s.width() > r.right()+1) { x = r.x(); y += height + spacing(); height=0; }
            if (apply) item->setGeometry(QRect(QPoint(x,y),s));
            x += s.width()+spacing(); height=qMax(height,s.height());
        }
        return y-r.y()+height;
    }
    QList<QLayoutItem *> m_items;
};
}
