#pragma once
#include "WorkspaceList.h"
#include <QTimer>
#include <QElapsedTimer>
#include <QPersistentModelIndex>
#include <QRegion>

namespace SessionRoles {
enum Role { Key = Qt::UserRole, Title, Meta, Status, Detail, Agent, Host, Children,
            Group, Header, Identity, Collapsed, Total, Attention, Working, Model, Effort, MachineColor, MachineName, Unread,
            ChildId, ParentKey, HasChildren, Expanded, ProjectColor, ProjectVivid, LaunchId, WorkingSince, ReviewLater, Draft, Failure,
            GitStatus, GitHost, GitPath, MetaIcon, Scheduled, OptionalQuestions, ModifiedAt, CoordinationWait, Waiting };
}

// Drops express placement relative to an identity, never a filtered row number.
class SessionList : public WorkspaceList {
    Q_OBJECT
public:
    explicit SessionList(QWidget *parent = nullptr);
    void setActivityAnimationEnabled(bool enabled);
    void syncActivityAnimation();
    bool activityAnimationRunning() const { return m_activityTimer.isActive(); }
    bool elapsedTimerRunning() const { return m_elapsedTimer.isActive(); }
    const QRegion &currentPaintRegion() const { return m_paintRegion; }
    bool dragging() const { return m_dragging; }
    bool bulkSelecting() const { return m_bulkSelecting; }
    void clearBulkSelection();
    void setSelectionBar(QWidget *bar, bool visible);
    static QString childrenLabel(const QModelIndex &index);
    static QRect childrenControlRect(const QRect &row, const QModelIndex &index, QFont font);
signals:
    void bulkSelectionChanged();
    void groupToggled(const QString &group);
    void childrenToggled(const QString &parentKey);
    void sessionMoved(const QString &identity, const QString &group, const QString &before);
    void groupMoved(const QString &group, const QString &before);
    void dragFinished();
protected:
    QStringList mimeTypes() const override { return {QStringLiteral("application/x-hgs-session-placement")}; }
    QMimeData *mimeData(const QList<QListWidgetItem *> &) const override;
    void startDrag(Qt::DropActions) override;
    void dragEnterEvent(QDragEnterEvent *) override;
    void dragMoveEvent(QDragMoveEvent *) override;
    void dragLeaveEvent(QDragLeaveEvent *) override;
    void dropEvent(QDropEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    bool viewportEvent(QEvent *) override;
private:
    QRect childrenControlRect(const QListWidgetItem *item) const;
    QStyleOptionViewItem animationOption(const QModelIndex &index) const;
    QList<QPersistentModelIndex> m_visibleWorkingRows;
    QHash<QPersistentModelIndex, QRegion> m_animationRegions;
    QRegion m_paintRegion;
    QTimer m_activityTimer, m_elapsedTimer;
    QElapsedTimer m_activityClock;
    bool m_animateActivity = true;
    bool validDrop(const QMimeData *) const;
    struct Placement { QString group, before; QRect indicator; bool onGroup = false; };
    Placement placement(const QPoint &point, bool group) const;
    QPoint m_press;
    QString m_pressedGroup;
    QString m_pressedParent;
    bool m_dragging = false;
    QRect m_indicator;
    bool m_onGroup = false;
    QString m_token;
    bool m_bulkSelecting = false, m_togglePress = false;
    QWidget *m_selectionBar = nullptr;
};
