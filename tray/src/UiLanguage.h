#pragma once

#include <QComboBox>
#include <QCoreApplication>
#include <QLibraryInfo>
#include <QSettings>
#include <QTranslator>

// Adapted from the optional Russian catalog contribution in PR #6. Widgets
// without Q_OBJECT inherit a base tr() context; use only unambiguous catalog
// entries for that fallback and preserve explicit disambiguation.
class ZerusTranslator : public QTranslator {
public:
    using QTranslator::QTranslator;
    QString translate(const char *context, const char *source,
                      const char *disambiguation = nullptr, int n = -1) const override {
        QString text = QTranslator::translate(context, source, disambiguation, n);
        if (text.isEmpty() && (!disambiguation || !*disambiguation))
            text = QTranslator::translate("Zerus", source, nullptr, n);
        return text;
    }
};

inline QString zerusUiLanguage() {
    // English stays the default on every OS. An unsupported/old "system"
    // preference does not silently opt an installed desktop into translation.
    return QSettings().value("workspace/language", "en").toString() == "ru" ? "ru" : "en";
}

inline QComboBox *zerusLanguageChoice(QWidget *parent = nullptr) {
    auto *choice = new QComboBox(parent);
    choice->setObjectName("workspaceLanguage");
    choice->addItem(QStringLiteral("English"), "en");
    choice->addItem(QObject::tr("Russian (preview)"), "ru");
    choice->setCurrentIndex(choice->findData(zerusUiLanguage()));
    QObject::connect(choice, &QComboBox::currentIndexChanged, choice, [choice] {
        QSettings().setValue("workspace/language", choice->currentData());
    });
    return choice;
}

inline bool installZerusTranslation(QCoreApplication &app, ZerusTranslator &translator,
                                    QTranslator &qtTranslator) {
    if (zerusUiLanguage() != "ru") return true;
    if (!translator.load(":/hgs/translations/zerus_ru.qm")) return false;
    // Optional platform dialog translations; absence retains Qt's English.
    if (qtTranslator.load("qtbase_ru", QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        app.installTranslator(&qtTranslator);
    app.installTranslator(&translator);
    return true;
}
