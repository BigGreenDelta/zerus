#pragma once
#include "HgsClient.h"
#include <QComboBox>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QLabel>
#include <QPair>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWidget>
#include <functional>
#include <utility>

// When the phone notifies. hgs stores the choice in the shared swarm catalog;
// every Zerus desktop and the phone edit the same value.
class MobileDeliverySettings : public QWidget {
public:
    static QList<QPair<QString, QString>> options() {
        return {{"immediate", tr("Immediately, even if I'm at a computer")}, {"away", tr("As soon as I'm away")},
                {"away_5", tr("After 5 minutes away")}, {"away_10", tr("After 10 minutes away")},
                {"away_15", tr("After 15 minutes away")}, {"away_30", tr("After 30 minutes away")}};
    }
    // A stale worker is normally restarted by the next hgs swarm read.
    static constexpr qint64 WorkerNoticeMs = 30000;
    explicit MobileDeliverySettings(const QString &executable, QWidget *parent = nullptr, std::function<qint64()> now = {})
        : QWidget(parent), client(executable, this), clock(std::move(now)) {
        if (!clock) { monotonic.start(); clock = [this] { return monotonic.elapsed(); }; }
        setObjectName("mobileDeliverySettings");
        auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 12, 0); layout->setSpacing(10);
        auto *heading = new QLabel(tr("Phone notifications")); heading->setObjectName("heading"); layout->addWidget(heading);
        auto *question = new QLabel(tr("When I'm not at a computer, send notifications to my phone")); question->setWordWrap(true); layout->addWidget(question);
        choice = new QComboBox; choice->setObjectName("mobileDelivery"); choice->setAccessibleName(question->text());
        for (const auto &option : options()) choice->addItem(option.second, option.first);
        choice->setEnabled(false); question->setBuddy(choice); layout->addWidget(choice, 0, Qt::AlignLeft);
        status = new QLabel; status->setObjectName("mobileDeliveryStatus"); status->setWordWrap(true); layout->addWidget(status);
        outdated = new QLabel; outdated->setObjectName("mobileDeliveryOutdated"); outdated->setWordWrap(true); outdated->hide(); layout->addWidget(outdated);
#ifdef Q_OS_MACOS
        const QString restart = "launchctl kickstart -k gui/$(id -u)/com.hgdev.hgs-swarm";
#else
        const QString restart = "systemctl --user restart hgs-swarm.service";
#endif
        worker = new QLabel(tr("The sync service on this computer is outdated and must be restarted (%1, or sign out and back in). Until then this computer may stop syncing projects and this choice.").arg(restart));
        worker->setObjectName("mobileDeliveryWorker"); worker->setWordWrap(true); worker->setTextInteractionFlags(Qt::TextSelectableByMouse);
        worker->hide(); layout->addWidget(worker);
        auto *hint = new QLabel(tr("You count as away after two minutes without keyboard or mouse input on every computer running Zerus, or while their screens are locked. Your computers and the phone share this choice."));
        hint->setWordWrap(true); layout->addWidget(hint);
        connect(choice, &QComboBox::activated, this, [this](int index) { save(choice->itemData(index).toString()); });
        connect(&client, &HgsClient::swarmReady, this, [this](quint64 id, const QJsonObject &result) { received(id, result, {}); });
        connect(&client, &HgsClient::swarmFailed, this, [this](quint64 id, const QString &error) {
            received(id, {}, error.isEmpty() ? tr("hgs did not respond") : error);
        });
    }
    // Settings calls this when it opens; nothing runs while it stays closed.
    void refresh() { if (!setRequest) getRequest = client.requestSwarm({"preference", "get", "--json"}); }
    // The 5-second swarm read keeps the value current and names members whose
    // hgs is too old to receive it, or a local sync service that stays outdated.
    void setSwarmSnapshot(const QJsonObject &snapshot) {
        // Reads from an older hgs report no worker and leave the notice unchanged.
        if (snapshot.value("worker").isObject()) {
            const auto state = snapshot.value("worker").toObject();
            const bool stale = state.value("running").toBool() && !state.value("current").toBool();
            if (!stale) staleWorkerSince = -1;
            else if (staleWorkerSince < 0) staleWorkerSince = clock();
            worker->setVisible(staleWorkerSince >= 0 && clock() - staleWorkerSince >= WorkerNoticeMs);
        }
        const auto value = snapshot.value("preferences").toObject().value("mobile_delivery").toString();
        // A snapshot requested before a confirmed save may arrive after it; it
        // must not put the old value back. Differing values are ignored until one
        // agrees or a few poll periods pass.
        const bool stale = !confirmed.isEmpty() && value != confirmed && confirmedAge.isValid() && confirmedAge.elapsed() < 15000;
        if (value == confirmed || !confirmedAge.isValid() || confirmedAge.elapsed() >= 15000) confirmed.clear();
        if (!setRequest && !stale && choice->findData(value) >= 0 && (value != current || loadFailed)) {
            current = value; show(value);
            if (std::exchange(loadFailed, false)) status->clear();
        }
        // Commands without peers (or an older hgs) leave the outdated list unchanged.
        if (!snapshot.contains("peers")) return;
        QStringList names;
        const auto peers = snapshot.value("peers").toObject();
        for (auto it = peers.begin(); it != peers.end(); ++it)
            if (it.value().toObject().value("catalog_outdated").toBool()) names << it.key();
        // Only direct connections report their version; indirect members are
        // named on the computer that connects to them.
        outdated->setVisible(!names.isEmpty());
        outdated->setText(names.size() == 1
            ? tr("Update Zerus on %1, a computer this one synchronizes with directly. Until then it keeps syncing projects but does not receive this choice.").arg(names.first())
            : tr("Update Zerus on these computers this one synchronizes with directly: %1. Until then they keep syncing projects but do not receive this choice.").arg(names.join(", ")));
    }
private:
    void save(const QString &value) {
        if (setRequest || value == current) { show(current); return; }
        choice->setEnabled(false); status->setText(tr("Saving…"));
        setRequest = client.requestSwarm({"preference", "set", "mobile_delivery", value, "--json"});
    }
    void received(quint64 id, const QJsonObject &result, const QString &error) {
        const auto value = result.value("mobile_delivery").toString();
        if (id && id == setRequest) {
            setRequest = 0;
            if (error.isEmpty() && result.value("ok").toBool() && choice->findData(value) >= 0) {
                current = value; show(value); confirmed = value; confirmedAge.start();
                status->setText(tr("Saved. Your other computers and the phone receive it with the next sync."));
            } else {
                status->setText(tr("Could not save: %1").arg(error.isEmpty() ? tr("unexpected response from hgs") : error));
                show(current); refresh();
            }
            return;
        }
        if (!id || id != getRequest) return;
        getRequest = 0;
        if (!error.isEmpty() || choice->findData(value) < 0) {
            loadFailed = true; choice->setEnabled(false);
            status->setText(tr("Could not read the shared setting: %1").arg(error.isEmpty() ? tr("unexpected response from hgs") : error));
            return;
        }
        if (std::exchange(loadFailed, false)) status->clear();
        current = value; show(value);
    }
    void show(const QString &value) {
        const QSignalBlocker block(choice);
        const int index = choice->findData(value);
        if (index >= 0) choice->setCurrentIndex(index);
        choice->setEnabled(!setRequest && index >= 0);
    }
    HgsClient client;
    std::function<qint64()> clock;
    QElapsedTimer monotonic;
    qint64 staleWorkerSince = -1;
    QComboBox *choice; QLabel *status, *outdated, *worker;
    QString current;
    quint64 getRequest = 0, setRequest = 0;
    QString confirmed;
    QElapsedTimer confirmedAge;
    bool loadFailed = false;
};
