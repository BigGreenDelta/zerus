#include <QtTest>
#include <QComboBox>
#include <QFile>
#include <QJsonObject>
#include <QLabel>
#include <QTemporaryDir>
#include "MobileDeliverySettings.h"

class TestMobileDeliverySettings : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    QString m_hgs;
    void touch(const QString &name, const QByteArray &text = {}) {
        QFile file(m_dir.filePath(name)); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(text);
    }
    QStringList calls() const {
        QFile file(m_dir.filePath("calls")); if (!file.open(QIODevice::ReadOnly)) return {};
        return QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
    }
private slots:
    void init() {
        QVERIFY(m_dir.isValid());
        for (const auto *name : {"calls", "value", "old", "fail-set"}) QFile::remove(m_dir.filePath(name));
        m_hgs = m_dir.filePath("hgs");
        QFile script(m_hgs); QVERIFY(script.open(QIODevice::WriteOnly));
        script.write(R"PY(#!/usr/bin/env python3
import json, pathlib, sys
root = pathlib.Path(__file__).parent
args = sys.argv[1:]
with (root / 'calls').open('a') as file: file.write(' '.join(args) + '\n')
value = root / 'value'
if (root / 'old').exists() or args[:2] != ['swarm', 'preference']:
    print('usage: hgs swarm get | initialize | apply', file=sys.stderr); sys.exit(1)
if args[2:] == ['get', '--json']:
    print(json.dumps({'mobile_delivery': value.read_text() if value.exists() else 'away'}))
elif args[2:4] == ['set', 'mobile_delivery'] and args[5:] == ['--json']:
    if (root / 'fail-set').exists(): print('catalog is busy', file=sys.stderr); sys.exit(1)
    value.write_text(args[4]); print(json.dumps({'ok': True, 'mobile_delivery': args[4]}))
else:
    sys.exit(2)
)PY");
        script.close(); QVERIFY(script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    }
    void loadsTheSharedValueAndOffersSixChoices() {
        touch("value", "away_15");
        MobileDeliverySettings settings(m_hgs);
        auto *choice = settings.findChild<QComboBox *>("mobileDelivery"); QVERIFY(choice);
        QVERIFY(!choice->isEnabled()); QVERIFY(calls().isEmpty()); // nothing runs until Settings opens
        const QStringList values{"immediate", "away", "away_5", "away_10", "away_15", "away_30"};
        const QStringList labels{"Immediately, even if I'm at a computer", "As soon as I'm away", "After 5 minutes away",
                                 "After 10 minutes away", "After 15 minutes away", "After 30 minutes away"};
        QCOMPARE(choice->count(), 6);
        for (int i = 0; i < 6; ++i) { QCOMPARE(choice->itemData(i).toString(), values[i]); QCOMPARE(choice->itemText(i), labels[i]); }
        settings.refresh();
        QTRY_VERIFY(choice->isEnabled());
        QCOMPARE(choice->currentData().toString(), QString("away_15"));
        QCOMPARE(calls(), QStringList{"swarm preference get --json"});
    }
    void choosingSavesThroughHgsAndKeepsTheConfirmedValue() {
        MobileDeliverySettings settings(m_hgs); auto *choice = settings.findChild<QComboBox *>("mobileDelivery");
        settings.refresh(); QTRY_VERIFY(choice->isEnabled());
        choice->setCurrentIndex(3); choice->activated(3);
        QVERIFY(!choice->isEnabled());
        QTRY_VERIFY(choice->isEnabled());
        QCOMPARE(choice->currentData().toString(), QString("away_10"));
        QVERIFY(calls().contains("swarm preference set mobile_delivery away_10 --json"));
        QVERIFY(settings.findChild<QLabel *>("mobileDeliveryStatus")->text().startsWith("Saved"));
    }
    void failedSaveShowsTheErrorAndRestoresTheStoredValue() {
        MobileDeliverySettings settings(m_hgs); auto *choice = settings.findChild<QComboBox *>("mobileDelivery");
        settings.refresh(); QTRY_VERIFY(choice->isEnabled());
        touch("fail-set");
        choice->setCurrentIndex(0); choice->activated(0);
        auto *status = settings.findChild<QLabel *>("mobileDeliveryStatus");
        QTRY_VERIFY(status->text().contains("catalog is busy"));
        QTRY_COMPARE(choice->currentData().toString(), QString("away"));
        QTRY_VERIFY(choice->isEnabled());
        QVERIFY(status->text().contains("catalog is busy"));
    }
    void olderCliKeepsTheChoiceDisabled() {
        touch("old");
        MobileDeliverySettings settings(m_hgs); settings.refresh();
        QTRY_VERIFY(settings.findChild<QLabel *>("mobileDeliveryStatus")->text().contains("usage: hgs swarm get"));
        QVERIFY(!settings.findChild<QComboBox *>("mobileDelivery")->isEnabled());
    }
    void swarmSnapshotUpdatesTheValueAndNamesOutdatedMembers() {
        MobileDeliverySettings settings(m_hgs);
        auto *choice = settings.findChild<QComboBox *>("mobileDelivery");
        auto *outdated = settings.findChild<QLabel *>("mobileDeliveryOutdated");
        settings.setSwarmSnapshot({{"preferences", QJsonObject{{"mobile_delivery", "away_30"}}},
            {"peers", QJsonObject{{"mac", QJsonObject{{"node", "n1"}, {"catalog_outdated", true}}}, {"build", QJsonObject{{"node", "n2"}}}}}});
        QCOMPARE(choice->currentData().toString(), QString("away_30")); QVERIFY(choice->isEnabled());
        QVERIFY(!outdated->isHidden()); QVERIFY(outdated->text().contains("mac")); QVERIFY(!outdated->text().contains("build"));
        settings.setSwarmSnapshot({{"preferences", QJsonObject{{"mobile_delivery", "unknown"}}}, {"peers", QJsonObject{}}});
        QCOMPARE(choice->currentData().toString(), QString("away_30")); QVERIFY(outdated->isHidden());
    }
};

QTEST_MAIN(TestMobileDeliverySettings)
#include "test_mobiledeliverysettings.moc"
