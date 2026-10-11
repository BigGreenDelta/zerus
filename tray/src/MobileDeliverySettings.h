#pragma once
#include "HgsClient.h"
#include <QComboBox>
#include <QJsonObject>
#include <QLabel>
#include <QPair>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWidget>
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
    explicit MobileDeliverySettings(const QString &executable, QWidget *parent = nullptr) : QWidget(parent), client(executable, this) {
        setObjectName("mobileDeliverySettings");
        auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 12, 0); layout->setSpacing(10);
        auto *heading = new QLabel(tr("Phone notifications")); heading->setObjectName("heading"); layout->addWidget(heading);
        auto *question = new QLabel(tr("When I'm not at a computer, send notifications to my phone")); question->setWordWrap(true); layout->addWidget(question);
        choice = new QComboBox; choice->setObjectName("mobileDelivery"); choice->setAccessibleName(question->text());
        for (const auto &option : options()) choice->addItem(option.second, option.first);
        choice->setEnabled(false); question->setBuddy(choice); layout->addWidget(choice, 0, Qt::AlignLeft);
        status = new QLabel; status->setObjectName("mobileDeliveryStatus"); status->setWordWrap(true); layout->addWidget(status);
        outdated = new QLabel; outdated->setObjectName("mobileDeliveryOutdated"); outdated->setWordWrap(true); outdated->hide(); layout->addWidget(outdated);
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
    // hgs is too old to receive it.
    void setSwarmSnapshot(const QJsonObject &snapshot) {
        const auto value = snapshot.value("preferences").toObject().value("mobile_delivery").toString();
        if (!setRequest && choice->findData(value) >= 0 && value != current) { current = value; show(value); }
        QStringList names;
        const auto peers = snapshot.value("peers").toObject();
        for (auto it = peers.begin(); it != peers.end(); ++it)
            if (it.value().toObject().value("catalog_outdated").toBool()) names << it.key();
        outdated->setVisible(!names.isEmpty());
        outdated->setText(tr("Update Zerus on %1. Until then it keeps syncing projects but does not receive this choice.").arg(names.join(", ")));
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
                current = value; show(value);
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
    QComboBox *choice; QLabel *status, *outdated;
    QString current;
    quint64 getRequest = 0, setRequest = 0;
    bool loadFailed = false;
};
