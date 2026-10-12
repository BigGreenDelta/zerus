#include "SpeechSettings.h"
#include "SpeechClient.h"
#include <QAudioDevice>
#include <QComboBox>
#include <QJsonArray>
#include <QLabel>
#include <QMediaDevices>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QVBoxLayout>

SpeechSettings::SpeechSettings(const QString &executable, QWidget *parent) : QWidget(parent), m_client(new SpeechClient(executable, this))
{
    setObjectName("speechSettings");
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0,0,12,0); layout->setSpacing(14);
    auto *heading = new QLabel(tr("Voice input")); heading->setObjectName("heading"); layout->addWidget(heading);
    auto *hint = new QLabel(tr("Record a message and review the recognized text before sending. Audio is processed on the selected computer using its configured provider. This choice is independent of the agent's computer."));
    hint->setWordWrap(true); layout->addWidget(hint);
    layout->addWidget(new QLabel(tr("Speech computer")));
    m_host = new QComboBox; m_host->setObjectName("speechComputer"); layout->addWidget(m_host);
    layout->addWidget(new QLabel(tr("Recognition provider")));
    m_profile = new QComboBox; m_profile->setObjectName("speechProfile"); layout->addWidget(m_profile);
    layout->addWidget(new QLabel(tr("Microphone")));
    m_microphone = new QComboBox; m_microphone->setObjectName("speechMicrophone"); layout->addWidget(m_microphone);
    m_status = new QLabel; m_status->setObjectName("speechSettingsStatus"); m_status->setWordWrap(true); layout->addWidget(m_status);
    m_refresh = new QPushButton(tr("Refresh providers and microphones")); m_refresh->setObjectName("speechRefresh"); layout->addWidget(m_refresh,0,Qt::AlignLeft);
    auto *privacy = new QLabel(tr("Provider setup and credentials stay on the speech computer. Recordings are limited to two minutes. Results are saved in private local drafts; voice input never sends a message automatically."));
    privacy->setWordWrap(true); layout->addWidget(privacy); layout->addStretch();
    m_statistics = new QLabel; m_statistics->setObjectName("speechStatistics"); m_statistics->setWordWrap(true); layout->insertWidget(layout->count()-1, m_statistics);
    setPeers({}); microphones();
    connect(m_host, &QComboBox::currentIndexChanged, this, [this] {
        QSettings settings; settings.setValue("speech/host", m_host->currentData());
        settings.remove("speech/profile"); refresh();
    });
    connect(m_profile, &QComboBox::currentIndexChanged, this, [this] {
        if (m_profile->currentIndex() >= 0) QSettings().setValue("speech/profile", m_profile->currentData());
    });
    connect(m_microphone, &QComboBox::currentIndexChanged, this, [this] { QSettings().setValue("speech/microphone", m_microphone->currentData()); });
    connect(m_refresh, &QPushButton::clicked, this, &SpeechSettings::refresh);
    auto *devices = new QMediaDevices(this);
    connect(devices, &QMediaDevices::audioInputsChanged, this, &SpeechSettings::microphones);
}
void SpeechSettings::setPeers(const QStringList &peers)
{
    const QSignalBlocker block(m_host);
    const auto selected = QSettings().value("speech/host").toString();
    QStringList available = peers; if (!selected.isEmpty() && !available.contains(selected)) available.append(selected);
    QStringList current;
    for (int index = 1; index < m_host->count(); ++index) current.append(m_host->itemData(index).toString());
    if (m_host->count() && current == available && m_host->currentData().toString() == selected) return;
    m_host->clear(); m_host->addItem(tr("This computer"), QString());
    for (const auto &host : available) m_host->addItem(host, host);
    m_host->setCurrentIndex(qMax(0, m_host->findData(selected)));
}
void SpeechSettings::microphones()
{
    const QSignalBlocker block(m_microphone);
    const auto selected = QSettings().value("speech/microphone").toByteArray();
    m_microphone->clear(); m_microphone->addItem(tr("System default microphone"), QByteArray());
    for (const auto &device : QMediaDevices::audioInputs()) m_microphone->addItem(device.description(), device.id());
    if (!selected.isEmpty() && m_microphone->findData(selected) < 0) m_microphone->addItem(tr("Selected microphone (unavailable)"), selected);
    m_microphone->setCurrentIndex(qMax(0, m_microphone->findData(selected)));
}
void SpeechSettings::refresh()
{
    microphones();
    const auto generation = ++m_generation;
    m_refresh->setEnabled(false); m_profile->setEnabled(false); m_status->setText(tr("Checking speech providers…"));
    const auto host = m_host->currentData().toString();
    m_statistics->setText(tr("Loading this computer's voice statistics…"));
    m_client->request(host, {{"action", "stats"}}, [this, generation](const QJsonObject &reply, const QString &error) {
        if (generation != m_generation) return;
        if (!error.isEmpty()) { m_statistics->setText(tr("Voice statistics unavailable.")); return; }
        const auto totals = reply.value("totals").toObject();
        m_statistics->setText(tr("This computer (last %1 days)\n%2 recordings, %3 completed, %4 failed, %5 cancelled, %6 uncertain\n%7 minutes of audio, %8 seconds processing\nThese counters stay on the speech computer and contain no audio or recognized text.")
            .arg(reply.value("retention_days").toInt(90)).arg(totals.value("submitted").toInteger()).arg(totals.value("completed").toInteger())
            .arg(totals.value("failed").toInteger()).arg(totals.value("cancelled").toInteger()).arg(totals.value("uncertain").toInteger())
            .arg(totals.value("audio_ms").toDouble()/60000.,0,'f',1).arg(totals.value("processing_ms").toDouble()/1000.,0,'f',1));
    });
    m_client->request(host, {{"action", "providers"}}, [this, generation](const QJsonObject &reply, const QString &error) {
        if (generation != m_generation) return;
        m_refresh->setEnabled(true);
        const QSignalBlocker block(m_profile); const auto saved = QSettings().value("speech/profile").toString();
        m_profile->clear();
        int first = -1;
        for (const auto &value : reply.value("profiles").toArray()) {
            const auto profile = value.toObject(); const bool available = profile.value("available").toBool();
            // Keep item data as a QString: QSettings stores a QString, while a
            // QVariant containing QJsonValue does not compare equal in findData.
            m_profile->addItem(profile.value("name").toString() + (available ? QString() : tr(" (unavailable)")), profile.value("id").toString());
            if (available && first < 0) first = m_profile->count() - 1;
            if (auto *model = qobject_cast<QStandardItemModel *>(m_profile->model())) model->item(m_profile->count()-1)->setEnabled(available);
            m_profile->setItemData(m_profile->count()-1, profile.value("reason").toString(), Qt::ToolTipRole);
        }
        const int selected = m_profile->findData(saved);
        // A missing explicitly selected profile never silently selects another provider.
        if (!saved.isEmpty() && selected < 0) { m_profile->addItem(tr("Selected provider (unavailable)"), saved); m_profile->setCurrentIndex(m_profile->count()-1); }
        else m_profile->setCurrentIndex(selected >= 0 ? selected : first);
        if (saved.isEmpty() && first >= 0) QSettings().setValue("speech/profile", m_profile->currentData());
        m_profile->setEnabled(first >= 0);
        m_status->setText(!error.isEmpty() ? error : !reply.value("available").toBool() ? reply.value("reason").toString(tr("No speech providers are configured on this computer.")) : tr("Ready. Use Voice in the message toolbar to start recording."));
    });
}
