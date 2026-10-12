#pragma once
#include <QApplication>
#include <QBoxLayout>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QTimer>

// Keep dismissal last without changing roles, default buttons or signal wiring.
// Native layouts may put Apply or Help after Cancel, even with WinLayout.
class DialogButtons : public QObject {
public:
    static void install() {
        if (qApp && !qApp->findChild<QObject *>("dialogButtons", Qt::FindDirectChildrenOnly)) new DialogButtons;
    }
protected:
    bool eventFilter(QObject *object, QEvent *event) override {
        const auto type = event->type();
        if (type != QEvent::Show && type != QEvent::LayoutRequest
            && type != QEvent::StyleChange && type != QEvent::LayoutDirectionChange) return false;
        auto *buttons = qobject_cast<QDialogButtonBox *>(object);
        if (!buttons) return false;
        if (type == QEvent::Show || type == QEvent::LayoutRequest) arrange(buttons);
        else if (type == QEvent::StyleChange || type == QEvent::LayoutDirectionChange)
            QTimer::singleShot(0, buttons, [buttons] { arrange(buttons); });
        return false;
    }
private:
    DialogButtons() : QObject(qApp) { setObjectName("dialogButtons"); qApp->installEventFilter(this); }
    static void arrange(QDialogButtonBox *buttons) {
        if (buttons->orientation() != Qt::Horizontal) return;
        auto *dismiss = buttons->button(QDialogButtonBox::Cancel);
        if (!dismiss) dismiss = buttons->button(QDialogButtonBox::Close);
        auto *layout = qobject_cast<QBoxLayout *>(buttons->layout());
        if (!dismiss || !layout) return;
        buttons->setLayoutDirection(Qt::LeftToRight);
        layout->setDirection(QBoxLayout::LeftToRight);
        // Reordering a layout emits another LayoutRequest. An already arranged
        // box must stay untouched to avoid a continuous layout loop.
        if (layout->indexOf(dismiss) == layout->count() - 1) return;
        layout->removeWidget(dismiss); layout->addWidget(dismiss);
        QWidget *previous = nullptr;
        for (int index = 0; index < layout->count(); ++index) {
            auto *widget = layout->itemAt(index)->widget();
            if (!widget) continue;
            if (previous) QWidget::setTabOrder(previous, widget);
            previous = widget;
        }
    }
};
