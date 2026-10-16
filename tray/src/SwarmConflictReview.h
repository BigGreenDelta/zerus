#pragma once

#include <QJsonObject>
#include <QVariantMap>
#include <QWidget>

class FleetState;
class IdentityBadge;
class QAbstractItemDelegate;
class QLabel;
class QPushButton;
class QTableWidget;

// Presents exactly the causal versions being reviewed. A refresh that changes
// those versions clears the choice; resolution never adopts unseen changes.
class SwarmConflictReview : public QWidget {
    Q_OBJECT
public:
    // Item data role of the drawn form of a conflict, value or origin: projects
    // as their colored labels and computers as machine badges, as elsewhere.
    static constexpr int PresentationRole = Qt::UserRole + 1;
    explicit SwarmConflictReview(const FleetState &fleet, QWidget *parent = nullptr);
    QString conflictLabel(const QJsonObject &conflict, const QJsonObject &snapshot) const;
    // The conflict list's form of conflictLabel(), for PresentationRole.
    QVariantMap conflictPresentation(const QJsonObject &conflict, const QJsonObject &snapshot) const;
    // Draws items that carry PresentationRole; others keep the default look.
    static QAbstractItemDelegate *createDelegate(QObject *parent);
    void setConflict(const QJsonObject &conflict, const QJsonObject &snapshot);
    void setBusy(bool busy);
signals:
    void resolutionRequested(const QJsonObject &payload);
protected:
    void changeEvent(QEvent *event) override;
private:
    QString projectName(const QString &id, const QJsonObject &snapshot) const;
    QVariantMap projectMark(const QString &id, const QJsonObject &snapshot, const QString &name = {}, const QString &color = {}) const;
    QString machineName(const QString &id, const QJsonObject &snapshot, bool origin = false) const;
    QVariantMap machineMark(const QString &id, const QJsonObject &snapshot, bool origin = false) const;
    QString subject(const QJsonObject &field, const QJsonObject &snapshot, const QJsonObject &conflict = {}) const;
    QString subjectTitle(const QJsonObject &field, const QJsonObject &snapshot, const QJsonObject &conflict = {}) const;
    QString valueText(const QJsonObject &field, const QJsonValue &value) const;
    QVariantMap valuePresentation(const QJsonObject &field, const QJsonValue &value) const;
    QString consequence(const QJsonValue &value) const;
    void updateChoice();
    const FleetState &m_fleet;
    QJsonObject m_conflict, m_snapshot;
    QLabel *m_title, *m_subject, *m_explanation, *m_outcome;
    IdentityBadge *m_subjectMachine = nullptr;
    QTableWidget *m_choices;
    QPushButton *m_confirm;
    bool m_busy = false;
};
