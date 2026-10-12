#include "speech/SpeechRecorder.h"
#include <QCoreApplication>
#include <QPermissions>
#include <QProcess>
#include <QTest>
#import <AVFoundation/AVFoundation.h>

class TestMacMicrophonePermissions : public QObject {
    Q_OBJECT
private slots:
    void backendIsRegistered() {
        QVERIFY(SpeechRecorder::microphonePermissionBackendAvailable());
    }
    void readOnlyStatusMatchesNative() {
        Qt::PermissionStatus expected = Qt::PermissionStatus::Denied;
        switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio]) {
        case AVAuthorizationStatusNotDetermined: expected = Qt::PermissionStatus::Undetermined; break;
        case AVAuthorizationStatusAuthorized: expected = Qt::PermissionStatus::Granted; break;
        case AVAuthorizationStatusRestricted:
        case AVAuthorizationStatusDenied: break;
        }
        // Checking alone must not request permission or open a microphone.
        QCOMPARE(qApp->checkPermission(QMicrophonePermission{}), expected);
    }
    void productionApplicationImportsBackend() {
        QProcess symbols;
        symbols.start("/usr/bin/nm", {"-j", QString::fromUtf8(ZERUS_TRAY_BINARY)});
        QVERIFY(symbols.waitForFinished(10000));
        QCOMPARE(symbols.exitCode(), 0);
        QVERIFY(symbols.readAllStandardOutput().contains("qt_static_plugin_QDarwinMicrophonePermissionPlugin"));
    }
};
QTEST_GUILESS_MAIN(TestMacMicrophonePermissions)
#include "test_macos_microphone_permissions.moc"
