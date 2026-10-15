#pragma once
#include <QFrame>
#include <QList>
#include <QMetaType>
#include <QString>
#include <functional>

class QLineEdit;
class QListWidget;

// One target of the Go to popup (Ctrl+K). See the "Go to" section of
// docs/superpowers/specs/2026-10-15-keyboard-shortcuts-design.md.
struct GoToItem {
    enum Kind { Session, Project, Account, Machine, Section, Command };
    Kind kind = Session;
    QString id;       // session key, project id, account id, machine host, page id or command id
    QString host;     // machine for sessions and accounts
    QString title;    // primary text
    QString detail;   // secondary text: project · machine · status, provider · machine, …
    QString chord;    // native chord text for commands, else empty
    int recency = -1; // 0 = most recent session of this run, -1 = none
};
Q_DECLARE_METATYPE(GoToItem)

namespace GoTo {
// Case-insensitive subsequence match of query over text. < 0 means no match; an
// empty query scores 0.
int score(const QString &query, const QString &text);
// Filters and orders items for a query. A leading '>' limits the list to commands.
QList<GoToItem> rank(const QList<GoToItem> &items, const QString &query);
}

// Search field plus result list, shown as a popup near the top of its parent.
// Items come from a provider callback so the popup works without the window.
class GoToDialog : public QFrame {
    Q_OBJECT
public:
    explicit GoToDialog(std::function<QList<GoToItem>()> provider, QWidget *parent);
    // Fetches the items, clears the query and shows the popup at the top center of the parent.
    void popup();
signals:
    void activated(const GoToItem &item);
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void refresh();
    void activateCurrent();
    void moveCurrent(int delta);
    void fitToResults();

    std::function<QList<GoToItem>()> m_provider;
    QList<GoToItem> m_items;
    QLineEdit *m_query = nullptr;
    QListWidget *m_results = nullptr;
};
