#include "OpenCodeModelPicker.h"
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#include <QTextDocument>
#include <QTreeWidget>

class TestOpenCodeModelPicker : public QObject {
    Q_OBJECT
    QJsonObject m_contract;
    static QTreeWidgetItem *row(QTreeWidget *tree, const QString &value) {
        for (int g = 0; g < tree->topLevelItemCount(); ++g)
            for (int m = 0; m < tree->topLevelItem(g)->childCount(); ++m)
                if (tree->topLevelItem(g)->child(m)->data(0, Qt::UserRole).toString() == value) return tree->topLevelItem(g)->child(m);
        return nullptr;
    }
    // What a Qt tooltip shows: rich text when Qt::mightBeRichText says so, else the text itself.
    static QString shownTip(const QString &tip) {
        if (!Qt::mightBeRichText(tip)) return tip;
        QTextDocument document; document.setHtml(tip); return document.toPlainText();
    }
    // A ready catalog of `count` models p/m0… and `gone` unavailable favorites gone/g0….
    static QJsonObject numbered(int count, int gone) {
        QJsonArray models, favorites;
        for (int i = 0; i < count; ++i)
            models.append(QJsonObject{{"value", QString("p/m%1").arg(i)}, {"label", QString("Model %1").arg(i)}, {"provider", "p"}, {"provider_label", "P"}});
        for (int i = 0; i < gone; ++i) favorites.append(QJsonObject{{"value", QString("gone/g%1").arg(i)}, {"available", false}});
        return {{"catalog_state","ready"},{"models",models},{"models_truncated",false},{"favorites",favorites}};
    }
    QJsonObject catalog() const {
        auto models = m_contract["row"].toObject()["model_options"].toArray();
        models.append(QJsonObject{{"id","solo/solo-model"},{"value","solo/solo-model"},{"label","Solo model"},{"provider","solo"},
            {"provider_label","Solo fixture"},{"free",false},{"effort_options",QJsonArray{"low"}},{"effort_labels",QJsonArray{"low"}}});
        auto providers = m_contract["row"].toObject()["providers"].toArray();
        auto solo = providers.last().toObject(); solo["folder"] = true; providers.replace(providers.size() - 1, solo);
        return {{"catalog_state","ready"},{"model_options",m_contract["row"].toObject()["model_options"]},{"model_options_truncated",false},
                {"providers",providers},{"models",models},{"models_truncated",true},
                {"favorites",QJsonArray{QJsonObject{{"value","gone/missing"},{"available",false}}}}};
    }
private slots:
    void initTestCase() {
        QFile file(QStringLiteral(ZERUS_OPENCODE_PROVIDER_CATALOG)); QVERIFY(file.open(QIODevice::ReadOnly));
        m_contract = QJsonDocument::fromJson(file.readAll()).object(); QVERIFY(!m_contract.isEmpty());
    }
    void parsesTheSharedShortListInOrderWithMarks() {
        const auto models = OpenCodeModels::parse(m_contract["row"].toObject()["model_options"].toArray(), 256);
        QStringList values; for (const auto &m : models) values << m.value;
        QCOMPARE(values, QStringList({"fixture/local","keyed/paid-model","groq/openai/gpt-oss-20b","fixture/alternate",
            "ollama/qwen3-coder:30b","opencode/glm-5.3-flash-free","opencode/big-pickle","groq/groq/compound"}));
        QCOMPARE(OpenCodeModels::marks(models[0]), QString("Default"));
        QCOMPARE(OpenCodeModels::marks(models[1]), QString("Favorite"));
        QCOMPARE(OpenCodeModels::marks(models[5]), QString("Free"));
        QCOMPARE(models[2].efforts, QStringList({"low","medium","high"}));
        const auto groups = OpenCodeModels::group(models);
        QStringList labels; for (const auto &g : groups) labels << g.label;
        QCOMPARE(labels, QStringList({"Private keyless fixture","Keyed fixture","Groq","Ollama","OpenCode"}));
        QCOMPARE(groups[2].models.size(), 2); QCOMPARE(groups[2].models[1].value, QString("groq/groq/compound"));
    }
    void providersNameTheirConnection() {
        const auto providers = OpenCodeModels::providers(m_contract["row"].toObject()["providers"].toArray());
        QCOMPARE(providers.size(), 6);
        QCOMPARE(OpenCodeModels::kindText(providers[0].kind), QString("Free, no key"));
        QCOMPARE(OpenCodeModels::kindText(providers[3].kind), QString("Detected locally"));
        QCOMPARE(providers[4].credentials, 2); QCOMPARE(providers[5].activeLabel, QString("Work"));
        QCOMPARE(OpenCodeModels::kindText("future-kind"), QString("Connected"));
    }
    void searchMatchesEveryWord() {
        const auto models = OpenCodeModels::parse(m_contract["row"].toObject()["model_options"].toArray(), 256);
        const auto values = [&](const QString &q) { QStringList v; for (const auto &m : OpenCodeModels::filter(models, q)) v << m.value; return v; };
        QCOMPARE(values("gpt 20B"), QStringList({"groq/openai/gpt-oss-20b"}));
        QCOMPARE(values("opencode"), QStringList({"opencode/glm-5.3-flash-free","opencode/big-pickle"}));
        QCOMPARE(values("keyless alternate"), QStringList({"fixture/alternate"}));
        QCOMPARE(values("  ").size(), 8);
    }
    void invalidEntriesAreDropped() {
        const QJsonArray entries{QJsonObject{{"value","noslash"}}, QJsonObject{{"value",QString("p/") + QString(255,'m')}},
            QJsonObject{{"value","p/a\u0007"}}, QJsonObject{{"id","p/ok"},{"label","OK"}}, QJsonObject{{"value","p/ok"}},
            QJsonObject{{"value","p/e"},{"effort_options",QJsonArray{"low","has space","a/b","low"}}}};
        const auto models = OpenCodeModels::parse(entries, 256);
        QCOMPARE(models.size(), 2); QCOMPARE(models[0].value, QString("p/ok")); QCOMPARE(models[1].efforts, QStringList({"low"}));
    }
    void groupsMarksAndStarsRequestFavorites() {
        OpenCodeModelPicker picker; picker.setShortList(m_contract["row"].toObject()["model_options"].toArray(), false); picker.show();
        auto *tree = picker.findChild<QTreeWidget *>("openCodeModelList"); QVERIFY(tree);
        QCOMPARE(tree->topLevelItemCount(), 5);
        QVERIFY(row(tree, "fixture/local")->text(0).contains("Default"));
        QCOMPARE(row(tree, "keyed/paid-model")->text(1), QString("★")); QCOMPARE(row(tree, "fixture/alternate")->text(1), QString("☆"));
        QSignalSpy requested(&picker, &OpenCodeModelPicker::favoriteRequested), chosen(&picker, &OpenCodeModelPicker::chosen);
        emit tree->itemClicked(row(tree, "fixture/alternate"), 1);
        QCOMPARE(requested.size(), 1); QCOMPARE(requested[0], QVariantList({"fixture/alternate", true})); QCOMPARE(chosen.size(), 0);
        picker.setFavoritePending("fixture/alternate", true); QCOMPARE(row(tree, "fixture/alternate")->text(1), QString("…"));
        emit tree->itemClicked(row(tree, "fixture/alternate"), 1); QCOMPARE(requested.size(), 1);
        picker.setFavorite("fixture/alternate", true); QCOMPARE(row(tree, "fixture/alternate")->text(1), QString("★"));
        emit tree->itemClicked(row(tree, "groq/groq/compound"), 0); QCOMPARE(chosen.size(), 1); QCOMPARE(picker.current(), QString("groq/groq/compound"));
        picker.setFavoritesAvailable(false, "mac is offline.");
        emit tree->itemClicked(row(tree, "groq/groq/compound"), 1); QCOMPARE(requested.size(), 1);
        QCOMPARE(row(tree, "groq/groq/compound")->toolTip(1), QString("mac is offline."));
    }
    void allModelsRequestsTheCatalogOnceAndSearchesIt() {
        OpenCodeModelPicker picker; picker.setShortList(m_contract["row"].toObject()["model_options"].toArray(), true); picker.show();
        auto *notice = picker.findChild<QLabel *>("openCodeModelNotice"); auto *all = picker.findChild<QPushButton *>("openCodeAllModels");
        auto *search = picker.findChild<QLineEdit *>("openCodeModelSearch"); auto *tree = picker.findChild<QTreeWidget *>("openCodeModelList");
        QCOMPARE(notice->text(), QString("Some models are not in this short list. Use All models to search every model."));
        QSignalSpy requested(&picker, &OpenCodeModelPicker::catalogRequested), favorites(&picker, &OpenCodeModelPicker::favoriteRequested);
        all->click(); QCOMPARE(requested.size(), 1); QCOMPARE(notice->text(), QString("Loading all models…"));
        picker.setCatalog(catalog()); QVERIFY(picker.showingAll());
        QVERIFY(picker.values().contains("solo/solo-model"));
        QVERIFY(notice->text().startsWith("Showing the first 9 models."));
        bool folder = false; for (int g = 0; g < tree->topLevelItemCount(); ++g) folder |= tree->topLevelItem(g)->text(0) == "Solo fixture · This folder";
        QVERIFY(folder);
        search->setText("solo"); QTRY_COMPARE(picker.values(), QStringList({"solo/solo-model"}));
        search->clear(); QTRY_VERIFY(row(tree, "gone/missing")); QTreeWidgetItem *gone = row(tree, "gone/missing"); QCOMPARE(gone->text(1), QString("★"));
        emit tree->itemClicked(gone, 0); QVERIFY(picker.current() != "gone/missing");
        emit tree->itemClicked(gone, 1); QCOMPARE(favorites[0], QVariantList({"gone/missing", false}));
        all->click(); QVERIFY(!picker.showingAll()); all->click(); QCOMPARE(requested.size(), 1);
        QCOMPARE(picker.model("solo/solo-model")->efforts, QStringList({"low"}));
    }
    void anUnknownCurrentModelStaysListedAndFailuresExplain() {
        OpenCodeModelPicker picker; picker.setShortList(m_contract["row"].toObject()["model_options"].toArray(), false);
        picker.setCurrent("custom/model"); QCOMPARE(picker.values().first(), QString("custom/model"));
        picker.findChild<QPushButton *>("openCodeAllModels")->click();
        picker.setCatalogFailed("OpenCode is not running on mac");
        QCOMPARE(picker.findChild<QLabel *>("openCodeModelNotice")->text(), QString("OpenCode is not running on mac"));
        QVERIFY(!picker.showingAll()); QVERIFY(picker.values().contains("fixture/local"));
    }
    void largeCatalogsRenderAtMostFiveHundredMatches() {
        QJsonArray models;
        for (int i = 0; i < 2000; ++i)
            models.append(QJsonObject{{"value", QString("p/m%1").arg(i)}, {"label", QString("Model %1").arg(i)}, {"provider", "p"}, {"provider_label", "P"}});
        OpenCodeModelPicker picker; picker.setShortList({}, false);
        picker.findChild<QPushButton *>("openCodeAllModels")->click();
        picker.setCatalog({{"catalog_state","ready"},{"models",models},{"models_truncated",false}});
        QCOMPARE(picker.values().size(), 500);
        QCOMPARE(picker.findChild<QLabel *>("openCodeModelNotice")->text(), QString("Showing the first 500 matches. Search to narrow the list."));
        picker.findChild<QLineEdit *>("openCodeModelSearch")->setText("model 1999");
        QTRY_COMPARE(picker.values(), QStringList({"p/m1999"}));
    }
    void olderComputersOfferNeitherAllModelsNorStars() {
        QVERIFY(!OpenCodeModels::catalogSupported({{"model_options", QJsonArray{}}}));
        QVERIFY(OpenCodeModels::catalogSupported({{"model_options_truncated", false}}));
        QCOMPARE(OpenCodeModels::updateText("mac"), QString("Update Zerus on mac to search all models and star favorites."));
        QCOMPARE(OpenCodeModels::errorText("usage: hgs account ls | add ID --provider codex", "mac"), OpenCodeModels::updateText("mac"));
        QCOMPARE(OpenCodeModels::errorText("Account catalog is busy.", "mac"), QString("Account catalog is busy."));
        OpenCodeModelPicker picker; picker.setShortList(m_contract["row"].toObject()["model_options"].toArray(), false);
        picker.setAllModelsAvailable(false, OpenCodeModels::updateText("mac"));
        auto *all = picker.findChild<QPushButton *>("openCodeAllModels"); QVERIFY(!all->isEnabled());
        QCOMPARE(all->toolTip(), OpenCodeModels::updateText("mac"));
        QSignalSpy requested(&picker, &OpenCodeModelPicker::catalogRequested); all->click(); QCOMPARE(requested.size(), 0);
    }
    // The complete `hgs account models` answer of the shared fixture: a folder
    // provider, an unavailable favorite, Enter to choose, and a forgotten catalog.
    void listsTheSharedAccountModelsAnswer() {
        const auto answer = m_contract["account_models"].toObject()["answer"].toObject(); QVERIFY(!answer.isEmpty());
        // hgs prints errors as "hgs: <message>"; an older hgs answers with its account usage.
        QCOMPARE(OpenCodeModels::errorText("hgs: usage: hgs account ls | add ID --provider codex", "mac"), OpenCodeModels::updateText("mac"));
        OpenCodeModelPicker picker; picker.setShortList(answer["model_options"].toArray(), answer["model_options_truncated"].toBool());
        picker.setCatalog(answer); picker.show(); QVERIFY(!picker.showingAll());
        auto *tree = picker.findChild<QTreeWidget *>("openCodeModelList");
        QVERIFY(!picker.values().contains("gone/missing"));
        QSignalSpy requested(&picker, &OpenCodeModelPicker::catalogRequested), chosen(&picker, &OpenCodeModelPicker::chosen);
        picker.findChild<QPushButton *>("openCodeAllModels")->click(); QCOMPARE(requested.size(), 0); QVERIFY(picker.showingAll());
        QVERIFY(picker.values().contains("workspace/team-coder"));
        QStringList headers; for (int g = 0; g < tree->topLevelItemCount(); ++g) headers << tree->topLevelItem(g)->text(0);
        QVERIFY(headers.contains("Workspace models · This folder")); QCOMPARE(headers.last(), QString("Unavailable favorites"));
        QCOMPARE(tree->topLevelItem(tree->topLevelItemCount() - 1)->childCount(), 1);
        QCOMPARE(row(tree, "gone/missing")->text(1), QString("★")); QVERIFY(row(tree, "fixture/local")->text(0).contains("Default"));
        QCOMPARE(picker.model("workspace/team-coder")->efforts, QStringList({"low","high"}));
        // solo/solo-model is only in the catalog, not in the folder's short list.
        tree->setCurrentItem(row(tree, "solo/solo-model")); QTest::keyClick(tree, Qt::Key_Return);
        QCOMPARE(chosen.size(), 1); QCOMPARE(picker.current(), QString("solo/solo-model"));
        tree->setCurrentItem(row(tree, "gone/missing")); QTest::keyClick(tree, Qt::Key_Return); QCOMPARE(chosen.size(), 1);
        picker.setFavorite("gone/missing", false); QVERIFY(!row(tree, "gone/missing"));
        QCOMPARE(tree->topLevelItem(tree->topLevelItemCount() - 1)->text(0), QString("Workspace models · This folder"));
        // A context without a catalog: the current choice stays listed first, and All models asks again.
        picker.setCatalog({}); QVERIFY(!picker.showingAll()); QCOMPARE(picker.values().first(), QString("solo/solo-model"));
        QVERIFY(picker.values().contains("workspace/team-coder"));
        picker.findChild<QPushButton *>("openCodeAllModels")->click(); QCOMPARE(requested.size(), 1);
    }
    // R1: a pending star belongs to its context. The owner keeps the request and
    // ignores its late answer; the widget's reset must not block the next context.
    void aContextResetClearsPendingStarsButARefreshKeepsThem() {
        const auto options = m_contract["row"].toObject()["model_options"].toArray();
        OpenCodeModelPicker picker; picker.setShortList(options, false); picker.setCatalog(catalog()); picker.show();
        auto *tree = picker.findChild<QTreeWidget *>("openCodeModelList");
        QSignalSpy requested(&picker, &OpenCodeModelPicker::favoriteRequested);
        picker.setFavoritePending("fixture/alternate", true);
        // An ordinary refresh of the same context keeps the pending star.
        picker.setCatalog(catalog()); QCOMPARE(row(tree, "fixture/alternate")->text(1), QString("…"));
        emit tree->itemClicked(row(tree, "fixture/alternate"), 1); QCOMPARE(requested.size(), 0);
        // Context B lists the same provider/model; A's late answer never reaches the picker.
        picker.setCatalog({}); picker.setShortList(QJsonArray{options.at(3)}, false);
        QCOMPARE(picker.values(), QStringList({"fixture/alternate"}));
        QCOMPARE(row(tree, "fixture/alternate")->text(1), QString("☆"));
        emit tree->itemClicked(row(tree, "fixture/alternate"), 1);
        QCOMPARE(requested.size(), 1); QCOMPARE(requested[0], QVariantList({"fixture/alternate", true}));
        picker.setFavoritePending("fixture/alternate", true); QCOMPARE(row(tree, "fixture/alternate")->text(1), QString("…"));
        picker.setFavorite("fixture/alternate", true); QCOMPARE(row(tree, "fixture/alternate")->text(1), QString("★"));
    }
    // M1: unavailable favorites share the one 500-row budget.
    void unavailableFavoritesShareTheFiveHundredRowBudget() {
        OpenCodeModelPicker picker; picker.setShortList({}, false);
        picker.setCatalog(numbered(500, 64)); picker.findChild<QPushButton *>("openCodeAllModels")->click(); QVERIFY(picker.showingAll());
        auto *notice = picker.findChild<QLabel *>("openCodeModelNotice");
        QCOMPARE(picker.values().size(), 500); QVERIFY(!picker.values().contains("gone/g0"));
        QCOMPARE(notice->text(), QString("Showing the first 500 matches. Search to narrow the list."));
        picker.setCurrent("p/m499"); QCOMPARE(picker.values().size(), 500); QVERIFY(picker.values().contains("p/m499"));
        // An unavailable current favorite is listed first and still within the budget.
        picker.setCurrent("gone/g63"); QCOMPARE(picker.values().size(), 500); QCOMPARE(picker.values().first(), QString("gone/g63"));
        QCOMPARE(picker.values().count(QString("gone/g63")), 1);
        // Search reaches every unavailable favorite.
        picker.findChild<QLineEdit *>("openCodeModelSearch")->setText("gone");
        QTRY_COMPARE(picker.values().size(), 64); QCOMPARE(picker.values().first(), QString("gone/g63")); QCOMPARE(notice->text(), QString());
    }
    // M2: a current model past the cut stays visible, in its group, and selected.
    void aCurrentModelPastTheCutStaysVisible() {
        OpenCodeModelPicker picker; picker.setShortList({}, false); picker.setCurrent("p/m1500");
        picker.findChild<QPushButton *>("openCodeAllModels")->click(); picker.setCatalog(numbered(2000, 0));
        auto *tree = picker.findChild<QTreeWidget *>("openCodeModelList");
        QCOMPARE(picker.values().size(), 500); QVERIFY(picker.values().contains("p/m1500")); QVERIFY(!picker.values().contains("p/m499"));
        QCOMPARE(tree->topLevelItemCount(), 1); QCOMPARE(picker.values().last(), QString("p/m1500"));
        QVERIFY(tree->currentItem()); QCOMPARE(tree->currentItem()->data(0, Qt::UserRole).toString(), QString("p/m1500"));
        QCOMPARE(picker.findChild<QLabel *>("openCodeModelNotice")->text(), QString("Showing the first 500 matches. Search to narrow the list."));
    }
    // M3: an unavailable current favorite keeps its star, asks for removal and is never chosen.
    void anUnavailableCurrentFavoriteKeepsItsStarAndIsNeverChosen() {
        OpenCodeModelPicker picker; picker.setShortList(m_contract["row"].toObject()["model_options"].toArray(), false);
        picker.setCatalog(catalog()); picker.setCurrent("gone/missing"); picker.show();
        auto *tree = picker.findChild<QTreeWidget *>("openCodeModelList");
        QSignalSpy chosen(&picker, &OpenCodeModelPicker::chosen), favorites(&picker, &OpenCodeModelPicker::favoriteRequested);
        QCOMPARE(picker.values().first(), QString("gone/missing")); QCOMPARE(row(tree, "gone/missing")->text(1), QString("★"));
        QVERIFY(picker.model("gone/missing")->favorite);
        emit tree->itemClicked(row(tree, "gone/missing"), 0); picker.select("gone/missing");
        tree->setCurrentItem(row(tree, "gone/missing")); QTest::keyClick(tree, Qt::Key_Return);
        QCOMPARE(chosen.size(), 0); QCOMPARE(picker.current(), QString("gone/missing"));
        emit tree->itemClicked(row(tree, "gone/missing"), 1);
        QCOMPARE(favorites.size(), 1); QCOMPARE(favorites[0], QVariantList({"gone/missing", false}));
        // All models lists it once, still starred.
        picker.findChild<QPushButton *>("openCodeAllModels")->click(); QVERIFY(picker.showingAll());
        QCOMPARE(picker.values().count(QString("gone/missing")), 1); QCOMPARE(picker.values().first(), QString("gone/missing"));
        QCOMPARE(row(tree, "gone/missing")->text(1), QString("★"));
        // Removed, it stays listed as the current, now unknown, model.
        picker.setFavorite("gone/missing", false);
        QCOMPARE(picker.values().first(), QString("gone/missing")); QCOMPARE(row(tree, "gone/missing")->text(1), QString("☆"));
    }
    // M4: owners' reasons are bounded and shown as literal text, never as markup.
    void ownerReasonsShowAsBoundedLiteralText() {
        OpenCodeModelPicker picker; picker.setShortList(m_contract["row"].toObject()["model_options"].toArray(), false);
        auto *tree = picker.findChild<QTreeWidget *>("openCodeModelList"); auto *all = picker.findChild<QPushButton *>("openCodeAllModels");
        const QString markup("<b>x</b> & <img src=\"x\"> &lt;");
        picker.setFavoritesAvailable(false, markup);
        QCOMPARE(shownTip(row(tree, "fixture/local")->toolTip(1)), markup);
        QCOMPARE(row(tree, "fixture/local")->data(1, Qt::AccessibleTextRole).toString(), markup);
        picker.setAllModelsAvailable(false, "<i>older</i>\nhgs");
        QCOMPARE(shownTip(all->toolTip()), QString("<i>older</i> hgs")); QCOMPARE(all->accessibleDescription(), QString("<i>older</i> hgs"));
        const QString bounded = QString(399, 'y') + QString("…");
        picker.setAllModelsAvailable(false, QString(1000, 'y'));
        QCOMPARE(shownTip(all->toolTip()), bounded); QCOMPARE(all->accessibleDescription(), bounded);
        picker.setFavoritesAvailable(false, QString(1000, 'y'));
        QCOMPARE(shownTip(row(tree, "fixture/local")->toolTip(1)), bounded);
        QCOMPARE(row(tree, "fixture/local")->data(1, Qt::AccessibleTextRole).toString(), bounded);
        picker.setAllModelsAvailable(true); QVERIFY(all->accessibleDescription().isEmpty());
    }
    // M3: a stale short list still offers stale/x, and it is the current model, but the
    // ready catalog omits it and marks the favorite unavailable. That status wins in
    // both views and on every route, whatever the short list's own favorite flag says.
    void aReadyCatalogMarksAStaleShortListFavoriteUnavailable() {
        auto ready = catalog(); auto stored = ready["favorites"].toArray();
        stored.append(QJsonObject{{"value","stale/x"},{"available",false}}); ready["favorites"] = stored;
        const QString why("OpenCode does not offer stale/x here. Its star removes the favorite.");
        for (const bool starred : {false, true}) {
            auto options = m_contract["row"].toObject()["model_options"].toArray();
            options.append(QJsonObject{{"id","stale/x"},{"value","stale/x"},{"label","Stale model"},{"provider","stale"},
                {"provider_label","Stale fixture"},{"free",false},{"favorite",starred}});
            OpenCodeModelPicker picker; picker.setShortList(options, false); picker.setCatalog(ready); picker.setCurrent("stale/x"); picker.show();
            auto *tree = picker.findChild<QTreeWidget *>("openCodeModelList");
            QSignalSpy chosen(&picker, &OpenCodeModelPicker::chosen), favorites(&picker, &OpenCodeModelPicker::favoriteRequested);
            for (int view = 0; view < 2; ++view) {
                if (view == 1) { picker.findChild<QPushButton *>("openCodeAllModels")->click(); QVERIFY(picker.showingAll()); }
                QVERIFY(row(tree, "stale/x")); QCOMPARE(picker.values().count(QString("stale/x")), 1);
                // The short list keeps it in its provider group; All models lists it first as the off-list current model.
                QCOMPARE(row(tree, "stale/x")->parent()->text(0), QString("Stale fixture"));
                if (view == 1) QCOMPARE(picker.values().first(), QString("stale/x"));
                QCOMPARE(row(tree, "stale/x")->text(0), QString("Stale model · Unavailable"));
                QCOMPARE(row(tree, "stale/x")->data(0, Qt::AccessibleDescriptionRole).toString(), why);
                QCOMPARE(shownTip(row(tree, "stale/x")->toolTip(0)), why);
                QCOMPARE(row(tree, "stale/x")->text(1), QString("★")); QVERIFY(picker.model("stale/x")->favorite);
                emit tree->itemClicked(row(tree, "stale/x"), 0); picker.select("stale/x");
                tree->setCurrentItem(row(tree, "stale/x")); QTest::keyClick(tree, Qt::Key_Return);
                QCOMPARE(chosen.size(), 0); QCOMPARE(picker.current(), QString("stale/x"));
                emit tree->itemClicked(row(tree, "stale/x"), 1);
                QCOMPARE(favorites.size(), view + 1); QCOMPARE(favorites.last(), QVariantList({"stale/x", false}));
            }
        }
    }
    // The positive control: available models and favorites still choose and toggle on every route.
    void availableModelsAndFavoritesStillChooseAndToggle() {
        OpenCodeModelPicker picker; picker.setShortList(m_contract["row"].toObject()["model_options"].toArray(), false);
        picker.setCatalog(catalog()); picker.setCurrent("keyed/paid-model"); picker.show();
        auto *tree = picker.findChild<QTreeWidget *>("openCodeModelList");
        QSignalSpy chosen(&picker, &OpenCodeModelPicker::chosen), favorites(&picker, &OpenCodeModelPicker::favoriteRequested);
        for (int view = 0; view < 2; ++view) {
            if (view == 1) { picker.findChild<QPushButton *>("openCodeAllModels")->click(); QVERIFY(picker.showingAll()); }
            QCOMPARE(row(tree, "keyed/paid-model")->text(0), QString("Paid model · Favorite"));
            QVERIFY(row(tree, "keyed/paid-model")->data(0, Qt::AccessibleDescriptionRole).toString().isEmpty());
            emit tree->itemClicked(row(tree, "keyed/paid-model"), 1); QCOMPARE(favorites.last(), QVariantList({"keyed/paid-model", false}));
            emit tree->itemClicked(row(tree, "fixture/alternate"), 1); QCOMPARE(favorites.last(), QVariantList({"fixture/alternate", true}));
            emit tree->itemClicked(row(tree, "fixture/alternate"), 0); QCOMPARE(picker.current(), QString("fixture/alternate"));
            tree->setCurrentItem(row(tree, "groq/groq/compound")); QTest::keyClick(tree, Qt::Key_Return);
            QCOMPARE(picker.current(), QString("groq/groq/compound"));
            picker.select("keyed/paid-model"); QCOMPARE(picker.current(), QString("keyed/paid-model"));
            QCOMPARE(chosen.size(), 3 * (view + 1)); QCOMPARE(favorites.size(), 2 * (view + 1));
        }
    }
};

QTEST_MAIN(TestOpenCodeModelPicker)
#include "test_opencodemodelpicker.moc"
