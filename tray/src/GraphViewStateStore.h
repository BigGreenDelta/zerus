#pragma once
#include <QJsonObject>
#include <QObject>
#include <QSettings>
#include <QTimer>

// Device-local presentation only. All settings I/O runs on one ordered worker;
// neither graph metadata nor any native operation is stored here.
class GraphViewStateStore : public QObject {
    Q_OBJECT
  public:
    explicit GraphViewStateStore(QString key, QObject *parent = nullptr);
    ~GraphViewStateStore() override;
    void save(const QJsonObject &state);
    void flush();
  signals:
    void loaded(const QJsonObject &state, const QString &error);
    void saved(const QString &error);

  private:
    void write(bool final);
    QString m_key, m_organization, m_application;
    QSettings::Format m_format;
    QJsonObject m_pending;
    QTimer m_delay;
    bool m_dirty = false;
    bool m_failed = false;
    bool m_inFlight = false;
};
