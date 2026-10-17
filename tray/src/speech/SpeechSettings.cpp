#include "SpeechSettings.h"
#include "SpeechClient.h"
#include <QAudioDevice>
#include <QDesktopServices>
#include <QScreen>
#include <QScrollArea>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMediaDevices>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSignalBlocker>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

namespace {
const auto pendingKey = "speech/providerOperation";
QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }
bool validMachine(const QString &value) { return !QUuid(value).isNull() && QUuid(value).toString(QUuid::WithoutBraces) == value; }
QLabel *label(const QString &text, QWidget *parent = nullptr) {
    auto *result = new QLabel(text, parent); result->setWordWrap(true); result->setTextFormat(Qt::PlainText); return result;
}
QPushButton *button(const QString &text, const QString &name, QHBoxLayout *row) {
    auto *result = new QPushButton(text); result->setObjectName(name); result->setAutoDefault(false); row->addWidget(result); return result;
}

QString localComputerName() {
#ifdef Q_OS_MACOS
    return QObject::tr("This Mac");
#elif defined(Q_OS_WIN)
    return QObject::tr("This Windows computer");
#else
    return QObject::tr("This computer (Linux)");
#endif
}
QUrl documentationUrl(const QString &adapter = {}) {
    QUrl url(QStringLiteral("https://github.com/ufna/zerus/blob/main/docs/speech-setup.md"));
    if (adapter == "gigaam") url.setFragment("local-gigaam");
    else if (adapter == "openai_http") url.setFragment("openai-compatible-server");
    return url;
}
void openDocumentationLink(const QUrl &url, QLabel *failure, const std::function<bool(const QUrl &)> &open) {
    const bool opened = open(url);
    failure->setText(opened ? QString() : QObject::tr("Could not open the browser. Open this link manually:\n%1").arg(url.toString()));
    failure->setVisible(!opened);
}

// All editor values are transient. A stored key is represented only by its
// presence; replacement text is cleared as soon as its request is dispatched.
class ProfileEditor : public QDialog {
public:
    ProfileEditor(const QString &computer, const QJsonObject &service, const QJsonArray &adapters, const QJsonObject &saved, QWidget *parent, std::function<bool(const QUrl &)> openDocumentation)
        : QDialog(parent), original(saved) {
        setObjectName("speechProfileEditor"); setWindowTitle(saved.isEmpty() ? tr("Add speech provider") : tr("Edit speech provider"));
        auto *layout = new QVBoxLayout(this); layout->setSpacing(12);
        auto *host = label(tr("Processing computer: %1").arg(computer)); host->setObjectName("speechEditComputer"); layout->addWidget(host);
        auto *readiness = label(service.value("available").toBool()
            ? tr("Recognition service is available on %1. Test the saved profile to verify the selected provider.").arg(computer)
            : tr("Recognition service is not ready on %1. %2").arg(computer, service.value("reason").toString(tr("Prepare the speech service on this processing computer before testing."))));
        readiness->setObjectName("speechEditReadiness"); layout->addWidget(readiness);
        auto *formScroll = new QScrollArea; formScroll->setWidgetResizable(true); formScroll->setFrameShape(QFrame::NoFrame); layout->addWidget(formScroll, 1);
        auto *content = new QWidget; content->setObjectName("speechEditorContent"); auto *fields = new QVBoxLayout(content); fields->setContentsMargins(0,0,12,0); fields->setSpacing(12); formScroll->setWidget(content);
        form = new QFormLayout; form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow); fields->addLayout(form);
        name = new QLineEdit(saved.value("name").toString()); name->setObjectName("speechEditName"); name->setMaxLength(128); name->setPlaceholderText(tr("e.g. Russian on %1").arg(computer)); form->addRow(tr("Name"), name);
        id = new QLineEdit(saved.value("id").toString(saved.isEmpty() ? "provider-" + uuid().left(8) : QString()));
        id->setObjectName("speechEditId"); id->setMaxLength(64); id->setReadOnly(!saved.isEmpty()); form->addRow(tr("Profile ID"), id);
        type = new QComboBox; type->setObjectName("speechEditType"); form->addRow(tr("Type"), type);
        model = new QComboBox; model->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); model->setObjectName("speechEditModel"); form->addRow(tr("Model"), model);
        language = new QLineEdit(saved.value("language").toString("ru")); language->setObjectName("speechEditLanguage"); language->setMaxLength(2); language->setPlaceholderText(tr("Auto-detect")); form->addRow(tr("Language"), language);
        device = new QComboBox; device->setObjectName("speechEditDevice"); form->addRow(tr("Device"), device);
        url = new QLineEdit(saved.value("base_url").toString()); url->setObjectName("speechEditUrl"); url->setMaxLength(2048); url->setPlaceholderText("https://speech.example.net/v1"); form->addRow(tr("Base URL"), url);
        insecure = new QCheckBox(tr("Allow unencrypted HTTP for a local or private server")); insecure->setObjectName("speechEditInsecureHttp");
        insecure->setChecked(saved.value("allow_insecure_http").toBool()); form->addRow(QString(), insecure);
        credential = new QComboBox; credential->setObjectName("speechEditCredential");
        const bool configured = saved.value("credential").toObject().value("configured").toBool();
        credential->addItem(configured ? tr("Keep stored API key") : tr("No API key"), "keep");
        credential->addItem(tr("Replace API key"), "replace"); credential->addItem(tr("Remove API key"), "remove"); form->addRow(tr("API key"), credential);
        key = new QLineEdit; key->setObjectName("speechEditKey"); key->setEchoMode(QLineEdit::Password); key->setMaxLength(8192); form->addRow(tr("New API key"), key);
        help = label({}); help->setObjectName("speechEditHelp"); fields->addWidget(help);
        documentation = new QPushButton; documentation->setObjectName("speechEditDocumentation"); documentation->setAutoDefault(false); layout->addWidget(documentation, 0, Qt::AlignLeft);
        documentationFailure = label({}); documentationFailure->setObjectName("speechEditDocumentationError"); documentationFailure->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard); documentationFailure->hide(); layout->addWidget(documentationFailure);
        connect(documentation, &QPushButton::clicked, this, [this, openDocumentation = std::move(openDocumentation)] {
            openDocumentationLink(documentationUrl(type->currentData().toString()), documentationFailure, openDocumentation);
        });
        status = label({}); status->setObjectName("speechEditStatus"); layout->addWidget(status);
        check = new QPushButton(tr("Check operation status")); check->setObjectName("speechEditCheckOperation"); check->setAutoDefault(false); check->hide(); layout->addWidget(check, 0, Qt::AlignLeft);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel); layout->addWidget(buttons);
        save = buttons->button(QDialogButtonBox::Save); save->setObjectName("speechSaveProfile"); save->setAutoDefault(false);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        capabilities = adapters;
        for (const auto &value : adapters) {
            const auto adapter = value.toObject().value("id").toString();
            if (adapter == "gigaam") type->addItem(tr("Local GigaAM"), adapter);
            else if (adapter == "openai_http") type->addItem(tr("OpenAI-compatible server"), adapter);
        }
        const auto selected = type->findData(saved.value("adapter").toString()); if (selected >= 0) type->setCurrentIndex(selected);
        else if (!saved.isEmpty()) { type->addItem(tr("Unsupported provider type"), saved.value("adapter").toString()); type->setCurrentIndex(type->count()-1); }
        connect(type, &QComboBox::currentIndexChanged, this, [this] { configureType(); });
        connect(credential, &QComboBox::currentIndexChanged, this, [this] { key->clear(); validate(); });
        for (auto *field : {name, id, language, url, key}) connect(field, &QLineEdit::textChanged, this, [this] { validate(); });
        connect(model, &QComboBox::currentTextChanged, this, [this] { validate(); });
        connect(device, &QComboBox::currentTextChanged, this, [this] { validate(); });
        connect(insecure, &QCheckBox::toggled, this, [this] { validate(); });
        configureType();
        if (saved.value("adapter").toString() == "openai_http") model->setEditText(saved.value("model").toString());
        else if (!saved.isEmpty()) { const auto value = saved.value("model").toString(); int index = model->findText(value); if (index < 0) { model->addItem(value); index = model->count()-1; } model->setCurrentIndex(index); }
        const auto savedDevice = saved.value("device").toString("cpu"); int deviceIndex = device->findText(savedDevice);
        if (!saved.isEmpty() && !remote() && deviceIndex < 0) { device->addItem(savedDevice); deviceIndex = device->count()-1; } if (deviceIndex >= 0) device->setCurrentIndex(deviceIndex);
        validate(); resize(580, qMin(700, screen()->availableGeometry().height()-80));
    }
    QJsonObject profile() const {
        QJsonObject result{{"id", id->text().trimmed()}, {"name", name->text().trimmed()}, {"adapter", type->currentData().toString()},
            {"model", model->currentText().trimmed()}, {"language", language->text().trimmed()}};
        if (remote()) { result["base_url"] = url->text().trimmed(); result["allow_insecure_http"] = insecure->isChecked(); }
        else result["device"] = device->currentText();
        return result;
    }
    QJsonObject secret() const {
        const auto mode = remote() ? credential->currentData().toString() : QStringLiteral("remove");
        QJsonObject result{{"mode", mode}}; if (mode == "replace") result["value"] = key->text(); return result;
    }
    void setBusy(bool busy) { locked = busy; for (auto *field : QList<QWidget *>{name, id, type, model, language, device, url, insecure, credential, key}) field->setEnabled(!busy); validate(); }
    void validate() {
        const bool replace = remote() && credential->currentData().toString() == "replace";
        key->setEnabled(!locked && replace);
        const auto endpoint = QUrl(url->text().trimmed(), QUrl::StrictMode);
        const bool urlOkay = endpoint.isValid() && !endpoint.host().isEmpty() && endpoint.userInfo().isEmpty() && !endpoint.hasQuery() && !endpoint.hasFragment()
            && (endpoint.scheme() == "https" || (endpoint.scheme() == "http" && insecure->isChecked()));
        const bool valid = !name->text().trimmed().isEmpty() && QRegularExpression("^[A-Za-z0-9_-]{1,64}$").match(id->text()).hasMatch()
            && !model->currentText().trimmed().isEmpty() && (language->text().isEmpty() || QRegularExpression("^[a-z]{2}$").match(language->text()).hasMatch()) && type->currentIndex() >= 0
            && (remote() ? urlOkay : allowedModels.contains(model->currentText()) && allowedDevices.contains(device->currentText())) && (!replace || !key->text().isEmpty());
        save->setEnabled(!locked && !conflict && valid);
    }
    QJsonObject original;
    QLineEdit *name, *id, *language, *url, *key;
    QComboBox *type, *model, *device, *credential;
    QCheckBox *insecure;
    QLabel *help, *status;
    QPushButton *save, *check;
    bool conflict = false;
private:
    bool remote() const { return type->currentData().toString() == "openai_http"; }
    void configureType() {
        const auto selected = type->currentData().toString(); const QSignalBlocker block(model);
        model->clear(); device->clear(); allowedModels.clear(); allowedDevices.clear(); model->setEditable(remote());
        if (model->lineEdit()) model->lineEdit()->setMaxLength(128);
        for (const auto &value : capabilities) { const auto adapter = value.toObject(); if (adapter.value("id").toString() != selected) continue;
            for (const auto &entry : adapter.value("models").toArray()) { model->addItem(entry.toString()); allowedModels.insert(entry.toString()); }
            for (const auto &entry : adapter.value("devices").toArray()) { device->addItem(entry.toString()); allowedDevices.insert(entry.toString()); }
        }
        if (remote() && original.value("adapter").toString() == selected) model->setEditText(original.value("model").toString());
        for (QWidget *field : QList<QWidget *>{url, insecure, credential, key}) { field->setVisible(remote()); if (auto *caption = form->labelForField(field)) caption->setVisible(remote()); }
        device->setVisible(!remote()); form->labelForField(device)->setVisible(!remote());
        documentation->setText(remote() ? tr("HTTP transcription documentation") : selected == "gigaam" ? tr("Local GigaAM documentation") : tr("Speech provider documentation"));
        documentation->setToolTip(documentationUrl(selected).toString()); documentationFailure->clear(); documentationFailure->hide();
        name->setPlaceholderText(remote() ? tr("e.g. Team transcription server") : tr("e.g. Russian speech"));
        help->setText(remote() ? tr("The processing computer sends audio to the server's /audio/transcriptions endpoint. Use the server's model ID and a two-letter language code, or leave language empty.\n\nThe optional API key is saved on the processing computer. Prefer HTTPS; private HTTP requires explicit consent. Saving sends no audio.")
            : selected == "gigaam" ? tr("Local inference runs on the processing computer named above. Saving creates a profile; it does not install the service or download model weights.\n\nv3_e2e_rnnt is the default for Russian with punctuation; use ru for Russian.\n\nCPU uses the processor. CUDA requires a supported NVIDIA GPU and CUDA-enabled PyTorch. MPS requires a supported Apple GPU/runtime. Use Test to verify the chosen model and device.")
            : tr("This provider type cannot be edited by this version of Zerus. See the provider documentation before changing it."));
        validate();
    }
    QFormLayout *form;
    QPushButton *documentation;
    QLabel *documentationFailure;
    QJsonArray capabilities;
    QSet<QString> allowedModels, allowedDevices;
    bool locked = false;
};
}

SpeechSettings::SpeechSettings(const QString &executable, QWidget *parent, std::function<bool(const QUrl &)> openDocumentation)
    : QWidget(parent), m_client(new SpeechClient(executable, this)),
      m_openDocumentation(openDocumentation ? std::move(openDocumentation) : QDesktopServices::openUrl)
{
    setObjectName("speechSettings");
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0,0,12,0); layout->setSpacing(14);
    auto *heading = label(tr("Voice input")); heading->setObjectName("heading"); layout->addWidget(heading);
    auto *preview = new QCheckBox(tr("Preview before sending")); preview->setObjectName("speechPreviewBeforeSend");
    preview->setChecked(QSettings().value("speech/preview_before_send",false).toBool()); layout->addWidget(preview);
    layout->addWidget(label(tr("Show Stop instead of Send while recording audio or video. Stop retains a preview for review and a separate Send. This preference applies on this device.")));
    connect(preview,&QCheckBox::toggled,this,[](bool checked){QSettings().setValue("speech/preview_before_send",checked);});
    layout->addWidget(label(tr("Manage recognition providers on a processing computer. Recording starts on this device; Send voice uses the provider selected beside your recording.")));
    layout->addWidget(label(tr("Configure providers on")));
    m_host = new QComboBox; m_host->setObjectName("speechComputer"); layout->addWidget(m_host);
    m_profiles = new QListWidget; m_profiles->setObjectName("speechProfiles"); m_profiles->setMinimumHeight(120); m_profiles->setMaximumHeight(260); layout->addWidget(m_profiles);
    m_profileDetails = label({}); m_profileDetails->setObjectName("speechProfileDetails"); layout->addWidget(m_profileDetails);
    auto *actions = new QHBoxLayout; layout->addLayout(actions);
    m_add = button(tr("Add"), "speechAdd", actions); m_edit = button(tr("Edit"), "speechEdit", actions);
    m_test = button(tr("Test"), "speechTest", actions); m_remove = button(tr("Remove"), "speechRemove", actions);
    actions->addStretch(); m_useDefault = button(tr("Use as default"), "speechUseDefault", actions);
    layout->addWidget(label(tr("Test sends a short synthetic recording from the selected computer. It may load a prepared model or use the remote API; it does not use your microphone or send anything to an agent.")));
    m_default = label({}); m_default->setObjectName("speechDefault"); layout->addWidget(m_default);
    m_status = label({}); m_status->setObjectName("speechSettingsStatus"); layout->addWidget(m_status);
    m_result = label({}); m_result->setObjectName("speechOperationResult"); m_result->hide(); layout->addWidget(m_result);
    auto *documentation = new QPushButton(tr("Speech provider documentation")); documentation->setObjectName("speechDocumentation"); documentation->setToolTip(documentationUrl().toString()); layout->addWidget(documentation,0,Qt::AlignLeft);
    auto *documentationFailure = label({}); documentationFailure->setObjectName("speechDocumentationError"); documentationFailure->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard); documentationFailure->hide(); layout->addWidget(documentationFailure);
    connect(documentation, &QPushButton::clicked, this, [this, documentationFailure] { openDocumentationLink(documentationUrl(), documentationFailure, m_openDocumentation); });
    m_refresh = new QPushButton(tr("Refresh providers and microphones")); m_refresh->setObjectName("speechRefresh"); layout->addWidget(m_refresh,0,Qt::AlignLeft);
    m_pendingStatus = label({}); m_pendingStatus->setObjectName("speechPendingOperation"); layout->addWidget(m_pendingStatus);
    m_check = new QPushButton(tr("Check operation status")); m_check->setObjectName("speechCheckOperation"); layout->addWidget(m_check,0,Qt::AlignLeft);
    layout->addWidget(label(tr("Microphone on this device")));
    m_microphone = new QComboBox; m_microphone->setObjectName("speechMicrophone"); layout->addWidget(m_microphone);
    layout->addWidget(label(tr("Provider settings and credentials stay on their processing computer. Recordings are limited to two minutes. Original audio and recognized text remain in conversation history; your typed draft stays separate.")));
    m_statistics = label({}); m_statistics->setObjectName("speechStatistics"); layout->addWidget(m_statistics); layout->addStretch();
    m_pending = QJsonDocument::fromJson(QSettings().value(pendingKey).toByteArray()).object();
    if (!validMachine(m_pending.value("machine_id").toString()) || QUuid(m_pending.value("request_id").toString()).isNull()) m_pending = {};
    setPeers({}); microphones(); controls();
    connect(m_host, &QComboBox::currentIndexChanged, this, [this] { QSettings().setValue("speech/manage_host", m_host->currentData()); refresh(); });
    connect(m_profiles, &QListWidget::currentRowChanged, this, [this] { controls(); });
    connect(m_microphone, &QComboBox::currentIndexChanged, this, [this] { QSettings().setValue("speech/microphone", m_microphone->currentData()); });
    connect(m_refresh, &QPushButton::clicked, this, &SpeechSettings::refresh);
    connect(m_add, &QPushButton::clicked, this, [this] { editProfile(true); }); connect(m_edit, &QPushButton::clicked, this, [this] { editProfile(false); });
    connect(m_remove, &QPushButton::clicked, this, &SpeechSettings::removeProfile); connect(m_test, &QPushButton::clicked, this, &SpeechSettings::testProfile);
    connect(m_useDefault, &QPushButton::clicked, this, &SpeechSettings::useDefault); connect(m_check, &QPushButton::clicked, this, &SpeechSettings::checkOperation);
    auto *devices = new QMediaDevices(this); connect(devices, &QMediaDevices::audioInputsChanged, this, &SpeechSettings::microphones);
}
void SpeechSettings::setPeers(const QStringList &peers)
{
    const QSignalBlocker block(m_host); const QSettings settings;
    const auto selected = settings.value("speech/manage_host", settings.value("speech/host")).toString();
    QStringList available = peers; if (!selected.isEmpty() && !available.contains(selected)) available.append(selected);
    QStringList current; for (int index = 1; index < m_host->count(); ++index) current.append(m_host->itemData(index).toString());
    if (m_host->count() && current == available && m_host->currentData().toString() == selected) return;
    m_host->clear(); m_host->addItem(localComputerName(), QString());
    for (const auto &host : available) m_host->addItem(host, host);
    m_host->setCurrentIndex(qMax(0, m_host->findData(selected)));
}
void SpeechSettings::microphones()
{
    const QSignalBlocker block(m_microphone); const auto selected = QSettings().value("speech/microphone").toByteArray();
    m_microphone->clear(); m_microphone->addItem(tr("System default microphone"), QByteArray());
    for (const auto &device : QMediaDevices::audioInputs()) m_microphone->addItem(device.description(), device.id());
    if (!selected.isEmpty() && m_microphone->findData(selected) < 0) m_microphone->addItem(tr("Selected microphone (unavailable)"), selected);
    m_microphone->setCurrentIndex(qMax(0, m_microphone->findData(selected)));
}
QJsonObject SpeechSettings::selectedProfile() const { return m_profiles->currentItem() ? m_profiles->currentItem()->data(Qt::UserRole).toJsonObject() : QJsonObject{}; }
void SpeechSettings::controls()
{
    const auto profile = selectedProfile(); const bool ready = m_loaded && !m_busy && m_pending.isEmpty();
    m_profiles->setEnabled(m_loaded);
    QString details = profile.value("reason").toString();
    if (!profile.isEmpty()) {
        const auto location = profile.value("adapter").toString() == "openai_http" ? profile.value("base_url").toString() : tr("Device: %1").arg(profile.value("device").toString());
        details = location + tr("\nLanguage: %1").arg(profile.value("language").toString().isEmpty() ? tr("Auto-detect") : profile.value("language").toString()) + (details.isEmpty() ? QString() : "\n" + details);
    }
    m_profileDetails->setText(details); m_profileDetails->setVisible(m_loaded && !details.isEmpty());
    m_add->setEnabled(ready && !m_adapters.isEmpty()); m_edit->setEnabled(ready && !profile.isEmpty()); m_remove->setEnabled(ready && !profile.isEmpty());
    m_test->setEnabled(ready && !profile.isEmpty()); m_useDefault->setEnabled(m_loaded && !profile.isEmpty());
    m_check->setVisible(!m_pending.isEmpty()); m_check->setEnabled(!m_busy); m_pendingStatus->setVisible(!m_pending.isEmpty());
    if (!m_pending.isEmpty()) m_pendingStatus->setText(m_busy || m_waiting ? tr("Working on the provider request. You can keep using Zerus.")
        : tr("A provider request on %1 is not yet confirmed. Check its status; it will not be sent again automatically.").arg(m_pending.value("computer").toString()));
    const QSettings settings; const auto id = settings.value("speech/profile").toString(); const auto machine = settings.value("speech/machine_id").toString();
    QString text = tr("Use as default selects a provider for new recordings. Existing recordings keep their selected provider.");
    if (!id.isEmpty()) {
        if (machine == m_machine || (machine.isEmpty() && m_host->currentData() == settings.value("speech/host"))) {
            bool found = false; for (int i = 0; i < m_profiles->count(); ++i) { const auto item = m_profiles->item(i)->data(Qt::UserRole).toJsonObject(); if (item.value("id").toString() == id) { text = tr("Default for new recordings: %1").arg(item.value("name").toString()); found = true; break; } }
            if (!found) text = tr("The selected default provider is unavailable. Choose another explicitly or restore it on this computer.");
        } else text = tr("The default provider is configured on another computer. Browsing these providers does not change it.");
    }
    m_default->setText(text);
}
void SpeechSettings::refresh()
{
    microphones(); const auto generation = ++m_generation; const auto host = m_host->currentData().toString();
    const auto selected = m_selectAfterRefresh.isEmpty() ? selectedProfile().value("id").toString() : m_selectAfterRefresh; m_selectAfterRefresh.clear(); m_loaded = false; m_refresh->setEnabled(false); controls();
    m_status->setText(tr("Checking provider configuration…")); m_statistics->setText(tr("Loading this computer's voice statistics…"));
    m_client->request(host, {{"action", "stats"}}, [this, generation](const QJsonObject &reply, const QString &error) {
        if (generation != m_generation) return;
        if (!error.isEmpty()) { m_statistics->setText(tr("Voice statistics unavailable.")); return; }
        const auto totals = reply.value("totals").toObject();
        m_statistics->setText(tr("This computer (last %1 days)\n%2 recordings, %3 completed, %4 failed, %5 cancelled, %6 uncertain\n%7 minutes of audio, %8 seconds processing\nThese counters stay on the speech computer and contain no audio or recognized text.")
            .arg(reply.value("retention_days").toInt(90)).arg(totals.value("submitted").toInteger()).arg(totals.value("completed").toInteger())
            .arg(totals.value("failed").toInteger()).arg(totals.value("cancelled").toInteger()).arg(totals.value("uncertain").toInteger())
            .arg(totals.value("audio_ms").toDouble()/60000.,0,'f',1).arg(totals.value("processing_ms").toDouble()/1000.,0,'f',1));
    });
    m_client->request(host, {{"action", "config_list"}}, [this, generation, selected](const QJsonObject &reply, const QString &error) {
        if (generation != m_generation) return;
        m_refresh->setEnabled(true);
        if (!error.isEmpty() || !validMachine(reply.value("machine_id").toString()) || !reply.value("revision").isDouble() || reply.value("revision").toInteger(-1) < 0) {
            m_status->setText(!error.isEmpty() ? error : tr("The computer returned an invalid provider configuration. Update hgs and refresh.")); controls(); return;
        }
        m_machine = reply.value("machine_id").toString(); m_revision = reply.value("revision").toInteger(); m_adapters = reply.value("adapters").toArray(); m_service = reply.value("service").toObject();
        m_profiles->clear(); int row = -1;
        const QSettings settings; const auto preferred = settings.value("speech/machine_id").toString() == m_machine ? settings.value("speech/profile").toString() : QString();
        for (const auto &value : reply.value("profiles").toArray()) {
            const auto profile = value.toObject(); const auto id = profile.value("id").toString();
            auto *item = new QListWidgetItem(profile.value("name").toString() + "\n" + (profile.value("adapter").toString() == "openai_http" ? tr("HTTP server") : tr("Local GigaAM")) + "  " + profile.value("model").toString() + "  "
                + (profile.value("available").toBool() ? tr("Ready") : tr("Not ready")), m_profiles);
            item->setData(Qt::UserRole, profile); item->setToolTip(profile.value("reason").toString());
            if (id == selected || (selected.isEmpty() && id == preferred)) row = m_profiles->count()-1;
        }
        m_profiles->setCurrentRow(row >= 0 ? row : m_profiles->count() ? 0 : -1); m_loaded = true;
        const auto service = reply.value("service").toObject();
        m_status->setText(!service.value("available").toBool() ? service.value("reason").toString(tr("The recognition service is not ready. You can still add and edit providers."))
            : m_profiles->count() ? tr("Provider configuration is ready. Test a saved provider before using it.") : tr("No providers yet. Add Local GigaAM or an OpenAI-compatible server."));
        controls();
    });
}
void SpeechSettings::useDefault()
{
    const auto profile = selectedProfile(); if (!m_loaded || profile.isEmpty()) return;
    QSettings settings; settings.setValue("speech/machine_id", m_machine); settings.setValue("speech/profile", profile.value("id").toString()); settings.setValue("speech/host", m_host->currentData()); settings.setValue("speech/profile_revision", profile.value("revision").toVariant()); controls();
}
void SpeechSettings::editProfile(bool create)
{
    if (!m_loaded || !m_pending.isEmpty()) return;
    const auto original = create ? QJsonObject{} : selectedProfile(); if (!create && original.isEmpty()) return;
    const auto host = m_host->currentData().toString(); const auto machine = m_machine; const auto revision = m_revision;
    QStringList ids; for (int row = 0; row < m_profiles->count(); ++row) ids.append(m_profiles->item(row)->data(Qt::UserRole).toJsonObject().value("id").toString());
    ProfileEditor dialog(m_host->currentText(), m_service, m_adapters, original, this, m_openDocumentation);
    connect(dialog.check, &QPushButton::clicked, this, &SpeechSettings::checkOperation);
    connect(m_client, &SpeechClient::requestFinished, &dialog, [this, &dialog] { dialog.check->setVisible(!m_pending.isEmpty()); dialog.check->setEnabled(!m_busy); if (!m_pending.isEmpty() && !m_busy && !m_waiting) dialog.status->setText(tr("The operation is not confirmed. Check its status before making another change.")); });
    connect(dialog.save, &QPushButton::clicked, &dialog, [this, &dialog, host, machine, revision, ids, create] {
        if (!dialog.save->isEnabled()) return;
        if (create && ids.contains(dialog.profile().value("id").toString())) { dialog.status->setText(tr("That profile ID already exists. Edit the existing provider or choose another ID.")); return; }
        auto payload = QJsonObject{{"action", "profile_upsert"}, {"expected_revision", revision}, {"profile", dialog.profile()}, {"credential", dialog.secret()}};
        dialog.setBusy(true); dialog.status->setText(tr("Saving on the selected computer. If the result is not confirmed, use Check operation status.")); dialog.key->clear();
        dialog.check->show(); dialog.check->setEnabled(false);
        operation(host, machine, payload, [guard = QPointer<ProfileEditor>(&dialog)](const QJsonObject &reply, const QString &error) {
            if (!guard) return;
            if (error.isEmpty() && reply.value("state").toString() == "completed") { guard->accept(); return; }
            guard->conflict = reply.value("error_code").toString() == "revision_conflict";
            guard->setBusy(false); guard->status->setText(guard->conflict ? tr("The saved configuration changed. Your edits are kept here. Close and reopen the provider to review its current values before saving.") : error);
        });
    });
    dialog.exec();
}
void SpeechSettings::removeProfile()
{
    const auto profile = selectedProfile(); if (!m_loaded || !m_pending.isEmpty() || profile.isEmpty()) return;
    const auto host = m_host->currentData().toString(); const auto machine = m_machine; const auto revision = m_revision;
    QMessageBox confirm(QMessageBox::Warning, tr("Remove speech provider?"), tr("Remove %1 from %2? Existing voice messages and recordings are kept. A recording using this provider will need an explicit replacement before sending.")
        .arg(profile.value("name").toString(), m_host->currentText()), QMessageBox::NoButton, this);
    confirm.setTextFormat(Qt::PlainText); confirm.setObjectName("speechRemoveConfirm");
    auto *remove = confirm.addButton(tr("Remove provider"), QMessageBox::DestructiveRole); remove->setObjectName("speechConfirmRemove");
    auto *cancel = confirm.addButton(QMessageBox::Cancel); confirm.setDefaultButton(cancel); confirm.setEscapeButton(cancel); confirm.exec();
    if (confirm.clickedButton() != remove || !m_loaded || m_machine != machine || m_host->currentData().toString() != host || m_revision != revision || !m_pending.isEmpty()) return;
    operation(host, machine, {{"action", "profile_remove"}, {"expected_revision", revision}, {"profile_id", profile.value("id")}});
}
void SpeechSettings::testProfile()
{
    const auto profile = selectedProfile(); if (!m_loaded || !m_pending.isEmpty() || profile.isEmpty()) return;
    operation(m_host->currentData().toString(), m_machine, {{"action", "profile_test"}, {"profile_id", profile.value("id")}, {"expected_profile_revision", profile.value("revision")}});
}
void SpeechSettings::operation(const QString &host, const QString &machine, QJsonObject payload, OperationReply done)
{
    if (!m_pending.isEmpty() || m_busy) return;
    m_result->clear(); m_result->hide();
    const auto request = uuid(); payload["request_id"] = request; payload["expected_machine_id"] = machine;
    m_pending = {{"host", host}, {"computer", host.isEmpty() ? localComputerName() : host}, {"machine_id", machine}, {"request_id", request}, {"action", payload.value("action")}};
    QSettings settings; settings.setValue(pendingKey, QJsonDocument(m_pending).toJson(QJsonDocument::Compact)); settings.sync();
    if (settings.status() != QSettings::NoError) { m_pending = {}; if (done) done({}, tr("Could not save the operation identity. No provider request was sent.")); controls(); return; }
    m_operationDone = std::move(done); m_busy = true; m_waiting = false; m_statusPolls = 0; m_statusClock.start(); controls();
    m_client->request(host, payload, [this](const QJsonObject &reply, const QString &error) { operationResult(reply, error, true); });
}
void SpeechSettings::operationResult(const QJsonObject &reply, const QString &error, bool initial)
{
    m_busy = false; m_waiting = false;
    const bool notStarted = initial && reply.value("client_dispatch_started").isBool() && !reply.value("client_dispatch_started").toBool();
    const auto state = notStarted ? QStringLiteral("failed") : reply.value("state").toString();
    if (error.isEmpty() && reply.value("machine_id") == m_pending.value("machine_id") && reply.value("request_id") == m_pending.value("request_id")
        && state == "running" && m_statusPolls++ < 60 && m_statusClock.elapsed() < 90000) {
        m_waiting = true; const auto request = m_pending.value("request_id");
        QTimer::singleShot(1000, this, [this, request] { if (m_pending.value("request_id") == request && !m_busy) checkOperation(); });
    }
    if (!notStarted && (!error.isEmpty() || reply.value("machine_id") != m_pending.value("machine_id") || reply.value("request_id") != m_pending.value("request_id")
        || (state != "completed" && state != "failed") || reply.value("error_code").toString() == "operation_not_found")) { controls(); return; }
    const auto pending = m_pending; auto done = std::move(m_operationDone); m_operationDone = {}; m_pending = {};
    QSettings settings; settings.remove(pendingKey); settings.sync();
    const auto failure = notStarted ? error : state == "failed" ? reply.value("error").toString(tr("The provider request failed.")) : QString();
    if (done) done(reply, failure);
    if (pending.value("host").toString() == m_host->currentData().toString()) {
        if (state == "completed" && pending.value("action").toString() == "profile_upsert") m_selectAfterRefresh = reply.value("profile_id").toString();
        refresh();
    }
    m_result->setText(state == "completed" ? pending.value("action").toString() == "profile_test" ? tr("The provider test on %1 completed successfully.").arg(pending.value("computer").toString()) : tr("Provider configuration saved on %1.").arg(pending.value("computer").toString()) : failure); m_result->show();
    controls();
}
void SpeechSettings::checkOperation()
{
    if (m_pending.isEmpty() || m_busy) return;
    if (!m_statusClock.isValid()) m_statusClock.start();
    m_busy = true; controls();
    m_client->request(m_pending.value("host").toString(), {{"action", "config_status"}, {"operation_id", m_pending.value("request_id")}, {"expected_machine_id", m_pending.value("machine_id")}},
        [this](const QJsonObject &reply, const QString &error) { operationResult(reply, error); });
}
