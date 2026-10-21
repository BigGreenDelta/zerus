#pragma once
#include "OpenCodeModels.h"
#include <QJsonObject>
#include <QSet>
#include <QWidget>
#include <optional>
class QLabel; class QLineEdit; class QPushButton; class QTimer; class QTreeWidget; class QTreeWidgetItem;

// The grouped OpenCode model list with search, Free/favorite/default marks, a
// star per model and All models. Owners fetch catalogs and favorites; the
// picker only asks for them. It never starts OpenCode and never changes a
// favorite itself: a star click only emits favoriteRequested.
//
// setShortList keeps the stored catalog. EVERY context change (another
// computer, folder or session) begins with setCatalog({}), even when the new
// context already has a catalog, before its short list, catalog and current
// model are set. That forgets the catalog and a pending request, turns All
// models off and clears this widget's pending stars. The owner keeps its
// favorite request identities and ignores their late answers; a non-empty
// setCatalog for the same context (a refresh) keeps pending stars.
class OpenCodeModelPicker : public QWidget {
    Q_OBJECT
public:
    explicit OpenCodeModelPicker(QWidget *parent = nullptr);
    void setShortList(const QJsonArray &options, bool truncated);
    void setCatalog(const QJsonObject &catalog);           // `hgs account models` output
    void setCatalogFailed(const QString &reason);
    void setCurrent(const QString &value);                  // an unknown value stays listed
    QString current() const;
    QStringList values() const;                             // rows shown now, in display order
    std::optional<OpenCodeModels::Model> model(const QString &value) const;
    void select(const QString &value);                      // as a click on that row
    void setFavoritesAvailable(bool available, const QString &reason = {});
    void setAllModelsAvailable(bool available, const QString &reason = {});   // off: the button is disabled with `reason` as tooltip
    void setFavoritePending(const QString &value, bool pending);
    void setFavorite(const QString &value, bool favorite);
    bool showingAll() const;
signals:
    void chosen(const QString &value);
    void favoriteRequested(const QString &value, bool favorite);
    void catalogRequested();
protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    void render();
    void addGroup(const OpenCodeModels::Group &group, bool unavailable);
    void decorate(QTreeWidgetItem *item) const;
    void click(QTreeWidgetItem *item, int column);
    void choose(const QString &value);
    void selectCurrent();
    bool catalogReady() const;
    QTreeWidgetItem *row(const QString &value) const;
    QLineEdit *m_search;
    QTreeWidget *m_list;
    QPushButton *m_all;
    QLabel *m_notice;
    QTimer *m_searchTimer;
    std::optional<QJsonArray> m_shortSource;                // what setShortList last parsed
    QList<OpenCodeModels::Model> m_short, m_models;         // the short list; the catalog's models
    QList<OpenCodeModels::Provider> m_providers;
    QStringList m_unavailable;                              // favorites the ready catalog does not offer
    QSet<QString> m_pending;
    QString m_current, m_failure, m_catalogState, m_catalogReason;
    QString m_favoritesReason, m_favoritesTip;              // the bounded literal reason; its tooltip
    bool m_shortTruncated = false, m_hasCatalog = false, m_modelsTruncated = false, m_loading = false;
    bool m_favoritesAvailable = true, m_offList = false;
};
