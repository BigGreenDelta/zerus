#include <QtTest>
#include <QFile>
#include <QLabel>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTreeWidget>
#include "FakeCli.h"
#include "PhonePairing.h"

namespace {
const char *Device = "00000000-0000-4000-8000-0000000000d1";
}

class TestPhonePairing : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;
    QString m_hgs;
    void touch(const QString &name) { QFile file(m_dir.filePath(name)); QVERIFY(file.open(QIODevice::WriteOnly)); }
    QStringList calls() const {
        QFile file(m_dir.filePath("calls")); if (!file.open(QIODevice::ReadOnly)) return {};
        return QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts);
    }
    static QStringList children(const QTreeWidget *tree, int column) {
        QStringList out;
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            for (int j = 0; j < tree->topLevelItem(i)->childCount(); ++j) out << tree->topLevelItem(i)->child(j)->text(column);
        return out;
    }
private slots:
    void init() {
        QVERIFY(m_dir.isValid());
        for (const auto *name : {"calls", "revoked", "old-peer", "no-connection"}) QFile::remove(m_dir.filePath(name));
        m_hgs = m_dir.filePath("hgs");
        QVERIFY(FakeCli::install(m_hgs, R"PY(#!/usr/bin/env python3
import json, pathlib, sys, time
root = pathlib.Path(__file__).parent
args = sys.argv[1:]
with (root / 'calls').open('a') as file: file.write(' '.join(args) + '\n')
host = args.pop(0)[1:] if args and args[0].startswith('@') else ''
if args == ['--help']:
    print('usage: hgs [@host] <cmd>')
    if not (root / ('old-' + host)).exists():
        print('       hgs [@host] mobile phones|invite|revoke-phone <id> --json   phones')
elif args == ['machine', 'ls']:
    print(json.dumps({'local': 'desk', 'machines': [{'alias': 'peer', 'enabled': True}, {'alias': 'off', 'enabled': False}]}))
elif args == ['mobile', 'phones', '--json']:
    if host != 'peer' or (root / 'no-connection').exists():
        print(json.dumps({'configured': False}))
    else:
        devices = [{'id': '00000000-0000-4000-8000-0000000000d1', 'name': 'Zerus iOS', 'platform': 'ios', 'paired_at': 1700000000,
                    'last_seen_at': 1700000300, 'added_by': {'computer_id': 'c', 'name': 'Gateway'}},
                   {'id': '00000000-0000-4000-8000-0000000000d2', 'name': 'Zerus Android', 'platform': 'android', 'paired_at': None,
                    'last_seen_at': None, 'added_by': None}]
        if (root / 'revoked').exists(): devices = devices[1:]
        print(json.dumps({'configured': True, 'server_url': 'https://relay.example.test', 'workspace_id': 'w1',
                          'workspace_name': 'Example workspace', 'devices': devices}))
elif args == ['mobile', 'invite', '--json']:
    print(json.dumps({'workspace_id': 'w1', 'workspace_name': 'Example workspace', 'pair_code': 'synthetic_pair-code_0123456789',
                      'expires_at': float((root / 'expires').read_text()), 'server_url': 'https://relay.example.test',
                      'pair_url': 'zerus://pair?server=https%3A%2F%2Frelay.example.test&code=synthetic_pair-code_0123456789'}))
elif args[:2] == ['mobile', 'revoke-phone'] and args[3:] == ['--json']:
    (root / 'revoked').write_text(args[2]); print(json.dumps({'revoked': True, 'device_id': args[2]}))
else:
    print('unexpected', args, file=sys.stderr); sys.exit(2)
)PY"));
    }
    void encodesPairingLinksAsQrCodes() {
        int size = 0;
        const auto modules = PhonePairing::encode("zerus://pair?server=https%3A%2F%2Frelay.zerus.dev&code=" + QString(43, 'A'), &size);
        QVERIFY(size >= 21); QCOMPARE((size - 17) % 4, 0); QCOMPARE(int(modules.size()), size * size);
        // The top-left finder pattern: a dark 7-module edge, then the light separator.
        for (int x = 0; x < 7; ++x) QVERIFY(modules[size_t(x)]);
        QVERIFY(!modules[7]);
        QVERIFY(PhonePairing::encode({}, &size).empty()); QCOMPARE(size, 0);
    }
    void listsPhonesOfReachableWorkspacesAndSkipsDisabledComputers() {
        PhonePairing::Panel panel(FakeCli::program(m_hgs));
        auto *tree = panel.findChild<QTreeWidget *>("linkedPhones"); QVERIFY(tree);
        panel.refresh();
        QTRY_COMPARE(tree->topLevelItemCount(), 1);
        QCOMPARE(tree->topLevelItem(0)->text(0), QString("Example workspace · via peer"));
        QCOMPARE(children(tree, 0), (QStringList{"Zerus iOS", "Zerus Android"}));
        QCOMPARE(children(tree, 1), (QStringList{"iOS", "Android"}));
        QCOMPARE(children(tree, 4), (QStringList{"Gateway", "—"}));
        QCOMPARE(children(tree, 2).value(1), QString("—"));
        QVERIFY(panel.findChild<QPushButton *>("pairPhone")->isEnabled());
        QCOMPARE(panel.findChild<QLabel *>("phonesStatus")->text(), QString("2 phones"));
        const auto log = calls();
        QVERIFY(log.contains("mobile phones --json")); QVERIFY(log.contains("@peer mobile phones --json"));
        for (const auto &call : log) QVERIFY2(!call.startsWith("@off"), qPrintable(call));
        QCOMPARE(panel.targets().size(), 1); QCOMPARE(panel.targets().first().host, QString("peer"));
    }
    void revokesThroughTheSameRouteOnlyAfterConfirmation() {
        bool accept = false; QString asked;
        PhonePairing::Panel panel(FakeCli::program(m_hgs), nullptr, [&](const QString &phone, const QString &workspace) {
            asked = phone + " / " + workspace; return accept;
        });
        auto *tree = panel.findChild<QTreeWidget *>("linkedPhones"); auto *revoke = panel.findChild<QPushButton *>("revokePhone");
        panel.refresh();
        QTRY_COMPARE(children(tree, 0).size(), 2);
        QVERIFY(!revoke->isEnabled());
        tree->setCurrentItem(tree->topLevelItem(0)->child(0));
        QVERIFY(revoke->isEnabled());
        revoke->click();
        QCOMPARE(asked, QString("Zerus iOS / Example workspace"));
        QTest::qWait(200);
        for (const auto &call : calls()) QVERIFY(!call.contains("revoke-phone"));
        accept = true; revoke->click();
        QTRY_COMPARE(children(tree, 0), QStringList{"Zerus Android"});
        QVERIFY(calls().contains(QString("@peer mobile revoke-phone %1 --json").arg(Device)));
    }
    void anOlderComputerIsNeverAskedToRunMobile() {
        touch("old-peer");
        PhonePairing::Panel panel(FakeCli::program(m_hgs));
        auto *status = panel.findChild<QLabel *>("phonesStatus");
        panel.refresh();
        QTRY_VERIFY(status->text().contains("Update Zerus on peer to manage phones"));
        for (const auto &call : calls()) QVERIFY2(!call.startsWith("@peer mobile"), qPrintable(call));
        QVERIFY(status->text().contains("Settings → Mobile connection"));
        QVERIFY(!panel.findChild<QPushButton *>("pairPhone")->isEnabled());
    }
    void thePairingDialogShowsASingleUseCodeUntilItExpires() {
        double now = 1'000'000;
        QFile expires(m_dir.filePath("expires")); QVERIFY(expires.open(QIODevice::WriteOnly)); expires.write("1000600"); expires.close();
        PhonePairing::Dialog dialog(FakeCli::program(m_hgs), {{"peer", "w1", "Example workspace", "https://relay.example.test"}}, 0,
                                    nullptr, [&now] { return now; });
        auto *code = dynamic_cast<PhonePairing::QrCodeView *>(dialog.findChild<QWidget *>("phonePairingCode")); QVERIFY(code);
        QTRY_COMPARE(code->text(), QString("zerus://pair?server=https%3A%2F%2Frelay.example.test&code=synthetic_pair-code_0123456789"));
        QVERIFY(code->modules() >= 21);
        QCOMPARE(dialog.findChild<QLabel *>("phonePairingCodeText")->text(), QString("synthetic_pair-code_0123456789"));
        QCOMPARE(dialog.findChild<QLabel *>("phonePairingServer")->text(), QString("https://relay.example.test"));
        QCOMPARE(dialog.findChild<QLabel *>("phonePairingExpiry")->text(), QString("Expires in 10:00"));
        QVERIFY(calls().contains("@peer mobile invite --json"));
        auto *copy = dialog.findChild<QPushButton *>("copyPairingCode"); QVERIFY(copy->isEnabled());
        now = 1'000'601;
        QTRY_VERIFY_WITH_TIMEOUT(dialog.findChild<QLabel *>("phonePairingExpiry")->text().contains("expired"), 3000);
        QVERIFY(!copy->isEnabled());
        dialog.findChild<QPushButton *>("closePhonePairing")->click();
        QVERIFY(code->text().isEmpty());
    }
};

QTEST_MAIN(TestPhonePairing)
#include "test_phonepairing.moc"
