#pragma once
#include <QColorDialog>
#include <QEventLoop>
#include <QPointer>

// QDialog::exec starts a native modal session on Cocoa. It swallows clicks on
// the owner before Qt's popup dismissal can see them. Keep the popup non-modal
// while waiting for its explicit result, so an outside click can reject it.
inline int execWorkspaceColorPopup(QDialog &dialog)
{
    QEventLoop loop;
    const QPointer<QDialog> guard(&dialog);
    QObject::connect(&dialog, &QDialog::finished, &loop, &QEventLoop::quit);
    QObject::connect(&dialog, &QObject::destroyed, &loop, &QEventLoop::quit);
    dialog.setWindowModality(Qt::NonModal);
    dialog.setResult(QDialog::Rejected);
    dialog.show();
    if (guard && dialog.isVisible()) loop.exec();
    return guard ? guard->result() : QDialog::Rejected;
}

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
    int exec() override { return execWorkspaceColorPopup(*this); }
};
