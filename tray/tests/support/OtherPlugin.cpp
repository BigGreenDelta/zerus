// A valid Qt plugin for another interface: NativeBrowserPlugin::load() must
// reject it instead of treating it as a native browser factory.
#include <QObject>
#include <QtPlugin>

class OtherInterface {
public:
    virtual ~OtherInterface() = default;
    virtual int value() const = 0;
};
Q_DECLARE_INTERFACE(OtherInterface, "dev.zerus.hgs-tray.tests.OtherInterface/1")

class OtherPlugin final : public QObject, public OtherInterface {
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "dev.zerus.hgs-tray.tests.OtherInterface/1")
    Q_INTERFACES(OtherInterface)
public:
    int value() const override { return 1; }
};

#include "OtherPlugin.moc"
