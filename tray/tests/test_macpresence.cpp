#include <QtTest>
#include <cmath>
#include <optional>
#include "DesktopPresence.h"

class TestMacPresence : public QObject {
    Q_OBJECT
private slots:
    void samplesSynchronouslyWithoutPermissionPrompts() {
        auto source = makePresenceSource(); QVERIFY(source);
        std::optional<PresenceSample> result;
        connect(source.get(), &PresenceSource::sampled, this, [&result](const PresenceSample &sample) { result = sample; });
        source->sample();
        QVERIFY(result.has_value());
        if (result->available) QVERIFY(std::isfinite(result->idleSeconds) && result->idleSeconds >= 0);
    }
};

QTEST_GUILESS_MAIN(TestMacPresence)
#include "test_macpresence.moc"
