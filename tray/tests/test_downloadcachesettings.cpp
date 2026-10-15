#include <QtTest>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include "DownloadCache.h"
#include "DownloadCacheSettings.h"

class TestDownloadCacheSettings : public QObject {
    Q_OBJECT
    QTemporaryDir settings;
private slots:
    void initTestCase() {
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    }
    void policyIsExplicitAndDefaultsToNeverClear() {
        QSettings().clear(); QTemporaryDir dir; QSettings().setValue("downloads/folder", dir.path());
        QFile unrelated(dir.filePath("user.txt")); QVERIFY(unrelated.open(QIODevice::WriteOnly)); unrelated.write("user file"); unrelated.close();
        DownloadCacheSettings page; page.show();
        auto *mode = page.findChild<QComboBox *>("downloadCachePolicy");
        auto *size = page.findChild<QDoubleSpinBox *>("downloadCacheSize");
        auto *apply = page.findChild<QPushButton *>("applyDownloadCachePolicy");
        QCOMPARE(mode->currentText(), QString("Never clear")); QVERIFY(!size->isEnabled()); QVERIFY(!apply->isEnabled());
        mode->setCurrentIndex(1); size->setValue(2); QVERIFY(apply->isEnabled()); QCOMPARE(DownloadCache::limitBytes(), 0);
        QTimer::singleShot(0, &page, [&] {
            auto *confirm = page.findChild<QMessageBox *>("confirmDownloadCacheLimit"); QVERIFY(confirm);
            QCOMPARE(confirm->defaultButton(), confirm->button(QMessageBox::Cancel)); confirm->reject();
        });
        apply->click(); QCOMPARE(DownloadCache::limitBytes(), 0); QVERIFY(apply->isEnabled());
        QTimer::singleShot(0, &page, [&] {
            auto *confirm = page.findChild<QMessageBox *>("confirmDownloadCacheLimit"); QVERIFY(confirm);
            for (auto *button : confirm->buttons()) if (confirm->buttonRole(button) == QMessageBox::AcceptRole) button->click();
        });
        apply->click(); QCOMPARE(DownloadCache::limitBytes(), 2LL * 1024 * 1024 * 1024); QVERIFY(!apply->isEnabled());
        mode->setCurrentIndex(0); apply->click(); QCOMPARE(DownloadCache::limitBytes(), 0); QVERIFY(!size->isEnabled());
        QTRY_VERIFY(page.findChild<QLabel *>("downloadCacheUsage")->text().contains("0 files"));
        QVERIFY(unrelated.open(QIODevice::ReadOnly)); QCOMPARE(unrelated.readAll(), QByteArray("user file"));
    }
};
QTEST_MAIN(TestDownloadCacheSettings)
#include "test_downloadcachesettings.moc"
