#pragma once
#include <QColorDialog>

// Native color panels bypass Qt's popup dismissal. Keep the chooser in the
// workspace so an outside click rejects it without replaying that click.
class WorkspaceColorDialog final : public QColorDialog {
public:
    WorkspaceColorDialog(const QColor &initial, QWidget *parent)
        : QColorDialog(initial, parent)
    {
        setObjectName("workspaceColorDialog");
        setOption(QColorDialog::DontUseNativeDialog);
        setWindowFlags(Qt::Popup);
        setAttribute(Qt::WA_NoMouseReplay);
    }
};
