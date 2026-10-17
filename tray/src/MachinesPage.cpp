#include "MachinesPage.h"
#include <QScopedValueRollback>
#include "HgsClient.h"
#include "PhonePairing.h"
#include "MachineAppearance.h"
#include "LocalSessionHost.h"
#include "MachinePlatform.h"
#include "WorkspaceIcons.h"
#include "WorkspacePageHeader.h"
#include "IdentityBadge.h"
#include "WorkspaceList.h"
#include "WorkspaceColorDialog.h"
#include <QStyledItemDelegate>
#include <QCheckBox>
#include <QComboBox>
#include <QSignalBlocker>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QShowEvent>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace {
class MachineDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const override { return {220, 64}; }
    void paint(QPainter *p, const QStyleOptionViewItem &o, const QModelIndex &index) const override {
        p->save(); p->setClipRect(o.rect); const bool dark = o.widget->property("hgsDark").toBool();
        IdentityBadges::paintRow(p, o, dark);
        const QString text = index.data(Qt::DisplayRole).toString(), machine = text.section('\n', 0, 0);
        const QRect r = o.rect.adjusted(13, 10, -13, -10);
        IdentityBadges::paint(p, QRect(r.x(), r.y(), IdentityBadges::width(IdentityBadges::Machine, machine, o.font, r.width()), 18), IdentityBadges::Machine, machine, dark);
        QFont font = o.font; font.setPixelSize(11); p->setFont(font); p->setPen(QColor(dark ? "#a1adbb" : "#647386"));
        p->drawText(QRect(r.x(), r.y() + 24, r.width(), 18), Qt::AlignVCenter, QFontMetrics(font).elidedText(text.section('\n', 1), Qt::ElideRight, r.width()));
        p->restore();
    }
};
}

MachinesPage::MachinesPage(const QString &path, QWidget *parent) : QWidget(parent), m_client(new HgsClient(path, this))
{
    setObjectName("machinesPage");
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(20, 18, 20, 18); layout->setSpacing(14);
    auto *title = new QLabel(tr("Machines")); title->setObjectName("heading");
    m_refresh = new QPushButton(tr("Refresh")); m_refresh->setObjectName("refreshMachineProfiles");
    m_add = new QPushButton(tr("Add machine")); m_add->setObjectName("addMachine");
    layout->addLayout(WorkspacePageHeader::row(title, m_refresh, m_add));
    m_summary = new QLabel(tr("SSH connections and HGS setup")); m_summary->setObjectName("muted"); layout->addWidget(m_summary);
    auto *body = new QHBoxLayout; body->setSpacing(24);
    m_list = new WorkspaceList; m_list->setObjectName("machineProfiles"); m_list->setMinimumWidth(220); m_list->setMaximumWidth(300); m_list->setAccessibleName(tr("Machines")); body->addWidget(m_list, 1);
    m_list->setItemDelegate(new MachineDelegate(m_list)); m_list->setMouseTracking(true); m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setStyleSheet("QListWidget#machineProfiles { background:transparent; border:0; outline:0; } QListWidget#machineProfiles::item { padding:0; border:0; }");
    auto *editorWidget = new QWidget; editorWidget->setObjectName("machineEditor");
    auto *editor = new QVBoxLayout(editorWidget); editor->setContentsMargins(0, 0, 8, 0); editor->setSpacing(12);
    m_title = new QLabel; m_title->setObjectName("heading"); m_title->setTextFormat(Qt::PlainText); editor->addWidget(m_title);
    m_origin = new QLabel; m_origin->setObjectName("muted"); m_origin->setWordWrap(true); m_origin->setTextFormat(Qt::PlainText); editor->addWidget(m_origin);
    m_localHost = new QLabel; m_localHost->setObjectName("localSessionHost"); m_localHost->setWordWrap(true); m_localHost->setTextFormat(Qt::PlainText); m_localHost->setTextInteractionFlags(Qt::TextSelectableByMouse); m_localHost->hide(); editor->addWidget(m_localHost);
    auto *appearance = new QHBoxLayout;
    m_color = new QPushButton(tr("Label color…")); m_color->setObjectName("machineColor"); appearance->addWidget(m_color);
    m_colorStyle = new QComboBox; m_colorStyle->setObjectName("machineLabelStyle"); m_colorStyle->setAccessibleName(tr("Label style"));
    m_colorStyle->addItem(tr("Soft"), false); m_colorStyle->addItem(tr("Bright"), true); appearance->addWidget(m_colorStyle);
    m_colorReset = new QPushButton(tr("Automatic")); m_colorReset->setObjectName("resetMachineColor"); m_colorReset->setToolTip(tr("Use the default color for this machine")); appearance->addWidget(m_colorReset);
    m_colorHint = new QLabel(tr("Saved on this device")); m_colorHint->setObjectName("muted"); appearance->addStretch(); editor->addLayout(appearance);
    auto *preview = new QHBoxLayout; auto *previewTitle = new QLabel(tr("Preview")); previewTitle->setObjectName("muted"); preview->addWidget(previewTitle);
    m_colorPreview = new IdentityBadge(IdentityBadges::Machine); m_colorPreview->setObjectName("machineLabelPreview"); preview->addWidget(m_colorPreview); preview->addWidget(m_colorHint); preview->addStretch(); editor->addLayout(preview);
    m_effective = new QLabel; m_effective->setObjectName("effectiveMachineConnection"); m_effective->setWordWrap(true); m_effective->setTextFormat(Qt::PlainText); m_effective->setTextInteractionFlags(Qt::TextSelectableByMouse); editor->addWidget(m_effective);
    auto *form = m_form = new QFormLayout; form->setSpacing(12); form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_alias = new QLineEdit; m_alias->setObjectName("machineAlias"); m_alias->setPlaceholderText(tr("e.g. studio-mac")); form->addRow(tr("Name / SSH alias"), m_alias);
    m_hostname = new QLineEdit; m_hostname->setObjectName("machineHostname"); m_hostname->setPlaceholderText(tr("Address or existing SSH alias")); form->addRow(tr("Hostname / IP"), m_hostname);
    m_user = new QLineEdit; m_user->setObjectName("machineUser"); m_user->setPlaceholderText(tr("Use SSH configuration")); form->addRow(tr("SSH user"), m_user);
    m_port = new QSpinBox; m_port->setObjectName("machinePort"); m_port->setRange(0, 65535); m_port->setSpecialValueText(tr("SSH configuration (usually 22)")); form->addRow(tr("Port"), m_port);
    auto *keyRow = new QHBoxLayout; m_key = new QLineEdit; m_key->setObjectName("machineKey"); m_key->setPlaceholderText(tr("Use SSH agent / configuration")); keyRow->addWidget(m_key, 1);
    m_browse = new QPushButton(tr("Browse…")); m_browse->setObjectName("browseMachineKey"); keyRow->addWidget(m_browse); form->addRow(tr("Private key path"), keyRow);
    m_enabled = new QCheckBox(tr("Show sessions and include in background polling")); m_enabled->setObjectName("machineEnabled"); form->addRow(m_enabled); editor->addLayout(form);
    auto *actions = new QHBoxLayout;
    m_save = new QPushButton(tr("Save connection")); m_save->setObjectName("saveMachine");
    m_discardDraft=new QPushButton(tr("Discard draft"));m_discardDraft->setObjectName("discardNewMachine");
    m_check = new QPushButton(tr("Test connection")); m_check->setObjectName("checkMachine");
    m_ssh = new QPushButton(tr("Open SSH terminal")); m_ssh->setObjectName("sshMachine");
    actions->addWidget(m_save); actions->addWidget(m_discardDraft); actions->addWidget(m_check); actions->addWidget(m_ssh); actions->addStretch(); editor->addLayout(actions);
    auto *setup = new QHBoxLayout;
    m_enableLocal = new QPushButton(tr("Enable local sessions")); m_enableLocal->setObjectName("enableLocalSessions"); m_enableLocal->hide(); setup->addWidget(m_enableLocal);
    m_enableLocal->setToolTip(tr("Run agents on this computer too. Zerus stages the HGS CLI and a session host in your user profile."));
    m_install = new QPushButton(tr("Install / update HGS…")); m_install->setObjectName("installMachine"); setup->addWidget(m_install);
    m_sessions = new QPushButton(tr("View sessions")); m_sessions->setObjectName("machineSessions"); setup->addWidget(m_sessions);
    m_accounts = new QPushButton(tr("Agent accounts…")); m_accounts->setObjectName("machineAccounts"); setup->addWidget(m_accounts); setup->addStretch(); editor->addLayout(setup);
    m_status = new QLabel; m_status->setObjectName("machineNotice"); m_status->setTextFormat(Qt::PlainText); m_status->setWordWrap(true); m_status->setTextInteractionFlags(Qt::TextSelectableByMouse); editor->addWidget(m_status);
    m_log = new QPlainTextEdit; m_log->setObjectName("machineSetupLog"); m_log->setReadOnly(true); m_log->setMaximumBlockCount(1500); m_log->hide(); editor->addWidget(m_log, 1);
    editor->addStretch(); auto *footer = new QHBoxLayout;
    m_reset = new QPushButton(tr("Reset SSH overrides")); m_reset->setObjectName("resetMachine"); footer->addWidget(m_reset);
    m_remove = new QPushButton(tr("Remove from Zerus")); m_remove->setObjectName("removeMachine"); footer->addWidget(m_remove); footer->addStretch(); editor->addLayout(footer);
    auto *scroll = new QScrollArea; scroll->setObjectName("machineEditorScroll"); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame); scroll->setWidget(editorWidget);
    scroll->setAutoFillBackground(false); scroll->viewport()->setAutoFillBackground(false); editorWidget->setAutoFillBackground(false);
    body->addWidget(scroll, 3); layout->addLayout(body, 1);
    // Phones of every workspace this computer reaches, here or through a peer.
    layout->addWidget(new PhonePairing::Panel(path));
    connect(m_color, &QPushButton::clicked, this, [this] {
        const auto machine = appearanceMachine(); if (m_new || machine.isEmpty()) return;
        WorkspaceColorDialog dialog(MachineAppearance::color(machine), this);
        dialog.setWindowTitle(tr("Label color for %1").arg(machine));
        if (dialog.exec() != QDialog::Accepted) return;
        const auto color = dialog.selectedColor();
        if (!color.isValid()) return;
        MachineAppearance::setColor(machine, color); updateAppearance(); renderRows(); emit appearanceChanged();
    });
    connect(m_colorStyle, &QComboBox::currentIndexChanged, this, [this] {
        const auto machine = appearanceMachine(); if (machine.isEmpty()) return;
        MachineAppearance::setVivid(machine, m_colorStyle->currentData().toBool()); updateAppearance(); renderRows(); emit appearanceChanged();
    });
    connect(m_colorReset, &QPushButton::clicked, this, [this] {
        const auto machine = appearanceMachine(); if (m_new || machine.isEmpty()) return;
        MachineAppearance::resetColor(machine); updateAppearance(); renderRows(); emit appearanceChanged();
    });
    const auto discardDraft=[this]{m_new=false;m_selected="@local";renderRows();loadSelection();};
    connect(m_discardDraft,&QPushButton::clicked,this,[this,discardDraft]{if(discardChanges())discardDraft();});
    connect(m_refresh, &QPushButton::clicked, this, [this,discardDraft] {
        if(!discardChanges())return;if(m_new)discardDraft();reload();
    });
    connect(m_add, &QPushButton::clicked, this, [this] {
        if(m_new){m_alias->setFocus();return;}
        if (!discardChanges()) return;
        m_new = true; m_selected="@new"; renderRows();loadSelection(); m_alias->setFocus();
    });
    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (m_loading || row < 0) return;
        const auto next = m_list->item(row)->data(Qt::UserRole).toString();
        if(next==(m_new?QStringLiteral("@new"):m_selected))return;
        if (discardChanges()) { m_selected = next; m_new = false; renderRows();loadSelection(); }
        else renderRows();
        const auto key=m_selected;const bool draft=m_new;
        // The original mouse press still applies its selection after this
        // returns, also after a nested dialog, at the row now under its old
        // position: a discarded draft can scroll the list. Restore the
        // highlight once it finishes.
        QTimer::singleShot(0,this,[this,key,draft]{
            if(m_selected!=key||m_new!=draft)return;
            const QSignalBlocker blocked(m_list);
            m_list->setCurrentRow(m_list->currentRow(),QItemSelectionModel::ClearAndSelect);
        });
    });
    for (auto *field : {m_alias, m_hostname, m_user, m_key}) connect(field, &QLineEdit::textChanged, this, &MachinesPage::changed);
    connect(m_port, &QSpinBox::valueChanged, this, &MachinesPage::changed); connect(m_enabled, &QCheckBox::toggled, this, &MachinesPage::changed);
    connect(m_browse, &QPushButton::clicked, this, [this] {
        if (!m_key->isEnabled()) return;
        const auto file = QFileDialog::getOpenFileName(this, tr("Choose SSH private key")); if (!file.isEmpty()) m_key->setText(file);
    });
    connect(m_save, &QPushButton::clicked, this, &MachinesPage::save); connect(m_check, &QPushButton::clicked, this, &MachinesPage::check);
    connect(m_ssh, &QPushButton::clicked, this, [this] { emit sshTerminalRequested(alias()); });
    connect(m_install, &QPushButton::clicked, this, &MachinesPage::install);
    connect(m_enableLocal, &QPushButton::clicked, this, &MachinesPage::enableLocalSessions);
    connect(m_sessions, &QPushButton::clicked, this, [this] { emit viewSessionsRequested(m_selected == "@local" ? QString() : alias()); });
    connect(m_accounts, &QPushButton::clicked, this, [this] { emit accountsRequested(m_selected == "@local" ? QString() : alias()); });
    for (auto *button : {m_remove, m_reset}) connect(button, &QPushButton::clicked, this, [this, button] {
        const bool remove = button == m_remove;
        const auto target = alias(), selection = m_selected, revision = m_data.value("revision").toString();
        if (remove) {
            QMessageBox confirm(QMessageBox::Question, tr("Remove machine?"),
                tr("Remove %1 from this Zerus? Its agents keep running and its files stay unchanged.").arg(target), QMessageBox::Cancel, this);
            confirm.setObjectName("removeMachineConfirm"); confirm.setTextFormat(Qt::PlainText);
            auto *proceed = confirm.addButton(tr("Remove machine"), QMessageBox::DestructiveRole);
            confirm.setDefaultButton(QMessageBox::Cancel); confirm.setEscapeButton(QMessageBox::Cancel);
            confirm.exec(); if (confirm.clickedButton() != proceed) return;
            if (m_request || alias() != target || m_selected != selection || m_data.value("revision").toString() != revision) {
                report(tr("The connection changed. Review it before removing."), true); return;
            }
        }
        m_operation = remove ? "remove" : "reset";
        m_request = m_client->requestMachines({m_operation, target, "--revision", revision}); updateControls();
    });
    connect(m_client, &HgsClient::machinesReady, this, [this](quint64 request, const QJsonObject &data) {
        if (request == m_resolveRequest) {
            m_resolveRequest = 0;
            if (!m_new && m_selected == m_resolveAlias && data.value("alias").toString() == m_resolveAlias) {
                m_effectiveData = data.value("effective").toObject(); showEffectiveConnection(m_effectiveData);
            }
            return;
        }
        if (request != m_request) return;
        const QString operation = m_operation; m_request = 0;
        if (operation == "ls") {
            m_data = data;
            if (!m_pendingSelection.isEmpty()) { m_selected = m_pendingSelection; m_pendingSelection.clear(); m_new = false; }
            renderRows(); loadSelection();
        }
        else if (operation == "check") {
            if (data.value("ok").toBool()) report(tr("Connected: %1 %2, %3, %4").arg(data.value("os").toString(), data.value("arch").toString(), data.value("hgs").toString(), data.value("tmux").toString()));
            else if (data.value("ssh_ok").toBool()) {
                QStringList missing;
                if (data.value("hgs").toString().isEmpty()) missing << tr("HGS");
                if (data.value("tmux").toString().isEmpty()) missing << tr("tmux");
                if (data.value("cargo").toString().isEmpty()) missing << tr("Rust");
                report(data.value("os").toString().isEmpty() ? data.value("error").toString()
                    : tr("SSH connected: %1 %2. Missing: %3. Install the newest stable Rust and tmux in the SSH terminal if needed, then use Install / update HGS.").arg(data.value("os").toString(), data.value("arch").toString(), missing.isEmpty() ? tr("a working HGS installation") : missing.join(", ")), true);
            }
            else report(tr("SSH connection failed: %1\nOpen SSH terminal to check credentials or approve the host key, then test again.").arg(data.value("error").toString()), true);
        } else { m_dirty = false; m_new = false; emit machinesChanged(); reload(); }
        updateControls();
    });
    connect(m_client, &HgsClient::machinesFailed, this, [this](quint64 request, const QString &error) {
        if (request == m_resolveRequest) {
            m_resolveRequest = 0;
            if (!m_new && m_selected == m_resolveAlias) m_effective->setText(tr("Could not read effective SSH settings: %1\nYour connection overrides are shown below.").arg(error));
        } else if (request == m_request) { m_request = 0; report(error, true); updateControls(); }
    });
    connect(m_client, &HgsClient::sessionHostReady, this, [this](quint64 request, const QJsonObject &data) {
        if (request != m_hostRequest) return;
        m_hostRequest = 0; m_installing = false;
        const auto cli = data.value("cli").toObject().value("path").toString();
        // This GUI keeps its CLI until it restarts (spec D2).
        m_localNotice = cli.isEmpty() ? tr("Local sessions are enabled on this computer. Zerus runs the stable HGS CLI from its next start.")
                                      : tr("Local sessions are enabled on this computer. Zerus runs %1 from its next start.").arg(cli);
        report(m_localNotice); updateControls(); emit machinesChanged();
    });
    connect(m_client, &HgsClient::sessionHostFailed, this, [this](quint64 request, const QString &error) {
        if (request != m_hostRequest) return;
        m_hostRequest = 0; m_installing = false; report(error, true); updateControls();
    });
    connect(m_client, &HgsClient::machineSetupOutput, this, [this](const QString &text) {
        auto cursor = m_log->textCursor(); cursor.movePosition(QTextCursor::End); cursor.insertText(text); m_log->setTextCursor(cursor); m_log->ensureCursorVisible();
    });
    connect(m_client, &HgsClient::machineSetupFinished, this, [this](bool ok, const QString &error) {
        m_installing = false; updateControls();
        if (ok) { report(tr("HGS installed. Checking connection…")); emit machinesChanged(); check(); }
        else report(error, true);
    });
    setTheme(false); loadSelection();
}
QString MachinesPage::alias() const { return m_alias->text().trimmed(); }
bool MachinesPage::aliasExists() const {
    if (alias() == m_fleet.local().host || alias() == m_data.value("local").toString()) return true;
    for (const auto &value : m_data.value("machines").toArray())
        if (value.toObject().value("alias").toString() == alias()) return true;
    return false;
}
QJsonObject MachinesPage::connection() const {
    return {{"hostname", m_hostname->text().trimmed()}, {"user", m_user->text().trimmed()}, {"port", m_port->value() ? QJsonValue(m_port->value()) : QJsonValue(QJsonValue::Null)}, {"identity_file", m_key->text().trimmed()}, {"enabled", m_enabled->isChecked()}};
}
void MachinesPage::report(const QString &text, bool error) { m_status->setText(text); m_status->setStyleSheet(error ? "color:#d37c69;" : QString()); }
void MachinesPage::changed() { if (!m_loading) { m_dirty = true; if (!m_effectiveData.isEmpty()) showEffectiveConnection(m_effectiveData); updateControls(); } }
bool MachinesPage::discardChanges() {
    if (m_installing || m_request || m_discardPrompt) return false;
    if (!m_dirty) return true;
    const QScopedValueRollback<bool> prompt(m_discardPrompt,true);
    QMessageBox dialog(QMessageBox::Question,tr("Unsaved connection"),tr("Discard the unsaved changes to this connection?"),QMessageBox::Discard|QMessageBox::Cancel,this);
    dialog.setObjectName("unsavedMachineDialog");dialog.setDefaultButton(QMessageBox::Cancel);dialog.setEscapeButton(QMessageBox::Cancel);
    if(dialog.exec()!=QMessageBox::Discard)return false;
    m_dirty = false; return true;
}
void MachinesPage::reload() { if (m_request || m_installing || m_dirty) return; m_operation = "ls"; m_request = m_client->requestMachines(); updateControls(); }
void MachinesPage::selectMachine(const QString &host) {
    const QString target = host.isEmpty() || host == "@local" || host == m_fleet.local().host || host == m_data.value("local").toString() ? QStringLiteral("@local") : host;
    if (target == m_selected && !m_new) return;
    if (m_request) {
        if (m_operation == "ls" && !m_dirty && !m_installing) m_pendingSelection = target;
        return;
    }
    if (!discardChanges()) return;
    if (m_data.isEmpty() && target != "@local") { m_pendingSelection = target; reload(); return; }
    m_selected = target; m_new = false; renderRows(); loadSelection();
}
void MachinesPage::showEvent(QShowEvent *event) { QWidget::showEvent(event); reload(); }
void MachinesPage::setTheme(bool dark) {
    m_colorPreview->setTheme(dark);
    m_list->setProperty("hgsDark", dark); m_list->viewport()->update();
    WorkspacePageHeader::theme(m_refresh, m_add, tr("Refresh machines"), dark);
}
void MachinesPage::setFleet(const FleetState &fleet) {
    // The origin text also names Enable local sessions while it is offered.
    const bool console = fleet.local().localSessions != m_fleet.local().localSessions
        || LocalSessionHost::canEnable(fleet.local()) != LocalSessionHost::canEnable(m_fleet.local());
    // Windows: its platform and session host decide what this computer offers.
    const bool host = fleet.local().localHost != m_fleet.local().localHost
        || MachinePlatform::of(fleet.local().metrics, true) != MachinePlatform::of(m_fleet.local().metrics, true);
    m_fleet = fleet; renderRows(); updateAppearance();
    if (console && !m_loading) { if (m_selected == "@local" && !m_new && !m_dirty) loadSelection(); else updateControls(); }
    else if (host && !m_loading) updateControls();
}
void MachinesPage::renderRows() {
    const QSignalBlocker blocked(m_list);const QScopedValueRollback<bool> loading(m_loading,true);
    // A control console is listed for its color, but it is no machine of the
    // fleet: the counts match Overview, which leaves it out.
    const bool console = !m_fleet.local().localSessions;
    m_list->clear(); int online = m_fleet.local().ok && !console ? 1 : 0;
    const auto localName = m_fleet.local().host.isEmpty() ? m_data.value("local").toString() : m_fleet.local().host;
    auto *local = new QListWidgetItem(MachineAppearance::icon(localName), localName + (m_fleet.local().localSessions ? tr("\nThis machine") : tr("\nThis computer: control console")), m_list); local->setData(Qt::UserRole, "@local");
    for (const auto &value : m_data.value("machines").toArray()) {
        const auto row = value.toObject(); const auto name = row.value("alias").toString(); const auto *box = m_fleet.peer(name);
        const bool live = box && box->ok && QDateTime::currentMSecsSinceEpoch() - m_fleet.peerPolledAt(name) < FleetState::kPeerStaleMs;
        if (live) ++online;
        const QString state = !row.value("enabled").toBool() ? tr("Disabled") : !live ? tr("Not connected")
            : box->sessions.size() == 1 ? tr("Connected, 1 session") : tr("Connected, %1 sessions").arg(box->sessions.size());
        auto *item = new QListWidgetItem(MachineAppearance::icon(name), name + '\n' + state, m_list); item->setData(Qt::UserRole, name);
    }
    const int savedCount=m_list->count()-(console?1:0);
    if(m_new){auto *draft=new QListWidgetItem(tr("New machine\nUnsaved connection"),m_list);draft->setData(Qt::UserRole,"@new");draft->setToolTip(tr("This connection is a draft. Save it to add the machine to Zerus."));}
    const auto selected=m_new?QStringLiteral("@new"):m_selected;
    for (int i = 0; i < m_list->count(); ++i) if (m_list->item(i)->data(Qt::UserRole).toString() == selected) m_list->setCurrentRow(i);
    m_summary->setText(savedCount == 1 ? tr("%1 connected, 1 machine").arg(online) : tr("%1 connected, %2 machines").arg(online).arg(savedCount));
}
void MachinesPage::loadSelection() {
    m_loading = true; QJsonObject row;
    for (const auto &v : m_data.value("machines").toArray()) if (v.toObject().value("alias").toString() == m_selected) row = v.toObject();
    if (!m_new && m_selected != "@local" && row.isEmpty()) { m_selected = "@local"; renderRows(); }
    const bool local = m_selected == "@local"; const auto c = row.value("connection").toObject();
    const auto localName = m_fleet.local().host.isEmpty() ? m_data.value("local").toString() : m_fleet.local().host;
    m_title->setText(m_new ? tr("New machine") : local ? localName : m_selected);
    m_alias->setText(m_new?QString():local ? localName : m_selected); m_hostname->setText(c.value("hostname").toString()); m_user->setText(c.value("user").toString());
    m_port->setValue(c.value("port").toInt()); m_key->setText(c.value("identity_file").toString()); m_enabled->setChecked(c.value("enabled").toBool(true));
    // A control-only computer (hgs.exe on Windows) runs no agents of its own.
    m_origin->setText(local ? (m_fleet.local().localSessions ? tr("This device runs Zerus. Add a remote machine to manage its agents over SSH.")
                               : tr("This computer is a control console: agents run on the remote machines you add here, over SSH.")
                                 + (LocalSessionHost::canEnable(m_fleet.local()) ? tr(" Enable local sessions to run them on this computer too.") : QString())) :
        (row.value("source").toString() == "config" ? tr("This alias comes from HGS configuration. ") : QString()) +
        tr("SSH supplies inherited values from your OpenSSH configuration and defaults. Fields below are Zerus overrides; blank fields keep the inherited value.") + (m_data.value("environment_override").toBool() ? tr(" HGS_PEERS in the environment overrides the list of monitored machines.") : QString()));
    m_dirty = false; m_loading = false; m_log->hide(); report(local ? m_localNotice : QString()); updateControls(); resolveConnection();
}
QString MachinesPage::appearanceMachine() const {
    if (m_new) return {};
    return m_selected == "@local" ? (m_fleet.local().host.isEmpty() ? m_data.value("local").toString() : m_fleet.local().host) : m_selected;
}
void MachinesPage::updateAppearance() {
    const auto machine = appearanceMachine(); const bool available = !machine.isEmpty();
    m_color->setEnabled(available); m_colorReset->setEnabled(available); m_colorStyle->setEnabled(available);
    const QSignalBlocker blocked(m_colorStyle); m_colorStyle->setCurrentIndex(MachineAppearance::vivid(machine) ? 1 : 0);
    m_colorPreview->setValue(machine);
    m_color->setIcon(available ? MachineAppearance::icon(machine) : QIcon());
    m_colorHint->setText(available ? tr("Saved on this device") : tr("Save the machine to choose its color"));
}
void MachinesPage::resolveConnection() {
    // Resolution is independent of editing and may finish after the selection
    // changes. Ignore old request IDs; never populate edit values from SSH.
    m_resolveRequest = 0; m_resolveAlias.clear(); m_effectiveData = {};
    m_hostname->setPlaceholderText(tr("Address or existing SSH alias")); m_user->setPlaceholderText(tr("Use SSH configuration"));
    m_port->setSpecialValueText(tr("SSH configuration (usually 22)")); m_key->setPlaceholderText(tr("Use SSH agent / configuration"));
    m_effective->setVisible(!m_new && m_selected != "@local");
    if (m_new || m_selected == "@local") return;
    m_effective->setText(tr("Reading effective SSH connection…")); m_resolveAlias = m_selected;
    m_resolveRequest = m_client->requestMachines({"resolve", m_selected});
}
void MachinesPage::showEffectiveConnection(const QJsonObject &data) {
    QStringList keys; for (const auto &key : data.value("identity_files").toArray()) keys << key.toString();
    QStringList details{tr("%1: %2@%3, port %4").arg(m_dirty ? tr("Saved connection") : tr("Effective connection"), data.value("user").toString(), data.value("hostname").toString(), data.value("port").toString())};
    if (!keys.isEmpty()) details << tr("Key paths: %1").arg(keys.join(", "));
    if (!data.value("proxy_jump").toString().isEmpty()) details << tr("Jump host: %1").arg(data.value("proxy_jump").toString());
    else if (data.value("proxy_command").toBool()) details << tr("Proxy command configured in OpenSSH");
    if (m_dirty) details << tr("Unsaved edits are not included. Save to refresh these values.");
    m_effective->setText(details.join('\n'));
    // Placeholders describe inheritance without turning it into a saved override.
    m_hostname->setPlaceholderText(tr("Inherited: %1").arg(data.value("hostname").toString()));
    m_user->setPlaceholderText(tr("Inherited: %1").arg(data.value("user").toString()));
    m_port->setSpecialValueText(tr("Inherited: %1").arg(data.value("port").toString()));
    if (!keys.isEmpty()) m_key->setPlaceholderText(tr("Inherited: %1").arg(keys.first()));
}
void MachinesPage::updateControls() {
    const bool local = m_selected == "@local", busy = m_request || m_installing;
    const bool validAlias = QRegularExpression("^[A-Za-z0-9_.][A-Za-z0-9_.-]{0,79}$").match(alias()).hasMatch();
    const bool duplicate = m_new && aliasExists();
    m_alias->setReadOnly(!m_new); m_alias->setEnabled(!busy && !local);
    for (auto *field : {m_hostname, m_user, m_key}) field->setEnabled(!busy && !local);
    m_port->setEnabled(!busy && !local); m_enabled->setEnabled(!busy && !local); m_browse->setEnabled(!busy && !local);
    m_save->setEnabled(!busy && !local && validAlias && !duplicate && (m_dirty || m_new));
    m_discardDraft->setVisible(m_new);m_discardDraft->setEnabled(!busy);
    m_save->setToolTip(duplicate ? tr("This name already exists. Select the machine in the list to edit it.") : QString());
    for (auto *button : {m_check, m_ssh, m_install, m_reset, m_remove}) button->setEnabled(!busy && !local && !m_new && !m_dirty);
    bool hasInheritedOverride = false;
    for (const auto &value : m_data.value("machines").toArray()) {
        const auto row = value.toObject();
        if (row.value("alias").toString() == m_selected)
            hasInheritedOverride = row.value("inherited").toBool() && row.value("source").toString() == "local";
    }
    m_reset->setEnabled(m_reset->isEnabled() && hasInheritedOverride);
    m_reset->setToolTip(tr("Restore the SSH settings of a machine from the shared configuration."));
    m_sessions->setEnabled(!busy && !m_new); m_accounts->setEnabled(!busy && !m_new); m_add->setEnabled(!busy); m_list->setEnabled(!busy); m_refresh->setEnabled(!busy);
    // A control console has no sessions, accounts or SSH connection of its own.
    const bool console = local && !m_new && !m_fleet.local().localSessions;
    m_sessions->setVisible(!console); m_accounts->setVisible(!console);
    for (int row = 0; row < m_form->rowCount(); ++row) m_form->setRowVisible(row, !console);
    for (auto *button : {m_save, m_check, m_ssh, m_install, m_reset, m_remove}) button->setVisible(!console);
    // Windows (spec D16): this computer shows its session host, and a control
    // console whose CLI reports one can enable local sessions.
    const auto &host = m_fleet.local().localHost;
    const bool windowsHost = local && !m_new && !host.isEmpty()
        && MachinePlatform::of(m_fleet.local().metrics, true) == MachinePlatform::Style::Windows;
    m_enableLocal->setVisible(local && !m_new && LocalSessionHost::canEnable(m_fleet.local())); m_enableLocal->setEnabled(!busy);
    m_localHost->setVisible(windowsHost);
    if (windowsHost) {
        const auto state = host.value("state").toString();
        QStringList facts{state == "not-installed" ? tr("not installed") : state == "stopped" ? tr("stopped")
                          : state == "running" ? tr("running") : state == "unavailable" ? tr("unavailable") : state};
        if (host.value("pid").isDouble()) facts << tr("PID %1").arg(host.value("pid").toInteger());
        if (!host.value("conpty").toString().isEmpty()) facts << tr("%1 ConPTY").arg(host.value("conpty").toString());
        if (!host.value("integrity").toString().isEmpty()) facts << tr("%1 integrity").arg(host.value("integrity").toString());
        if (host.value("windows_session").isDouble()) facts << tr("Windows session %1").arg(host.value("windows_session").toInteger());
        const auto error = host.value("error").toString();
        m_localHost->setText(tr("Session host: %1").arg(facts.join(", ")) + (error.isEmpty() ? QString() : ": " + error));
    }
    updateAppearance();
}
void MachinesPage::save() {
    if (m_request || m_installing || (m_new && aliasExists())) return;
    m_selected = alias(); m_operation = "set";
    QStringList arguments{"set", alias(), "--json", QString::fromUtf8(QJsonDocument(connection()).toJson(QJsonDocument::Compact)), "--revision", m_data.value("revision").toString()};
    if (m_new) arguments << "--create";
    m_request = m_client->requestMachines(arguments); updateControls();
}
void MachinesPage::check() { m_operation = "check"; report(tr("Testing SSH and checking HGS…")); m_request = m_client->requestMachines({"check", alias()}); updateControls(); }
void MachinesPage::enableLocalSessions() {
    if (m_request || m_installing) return;
    QMessageBox confirm(QMessageBox::Question, tr("Enable local sessions?"),
        tr("Run agents on this computer too? Zerus stages the HGS CLI and a session host in your user profile and enables local sessions. "
           "To disable local sessions again, run hgs session-host uninstall."), QMessageBox::Cancel, this);
    confirm.setObjectName("enableLocalSessionsConfirm"); confirm.setTextFormat(Qt::PlainText);
    auto *proceed = confirm.addButton(tr("Enable local sessions"), QMessageBox::AcceptRole);
    confirm.setDefaultButton(QMessageBox::Cancel); confirm.setEscapeButton(QMessageBox::Cancel);
    confirm.exec(); if (confirm.clickedButton() != proceed || m_request || m_installing) return;
    m_installing = true; report(tr("Enabling local sessions on this computer…")); updateControls();
    m_hostRequest = m_client->requestSessionHost("install");
}
void MachinesPage::install() {
    QDialog dialog(this); dialog.setObjectName("machineSetupDialog"); dialog.setWindowTitle(tr("Install HGS on %1").arg(alias())); dialog.resize(520, 280);
    auto *layout = new QVBoxLayout(&dialog);
    auto *description = new QLabel(tr("Build and install the HGS CLI on %1 using SSH. The newest stable Rust and tmux must already be installed there. Existing sessions and configuration are preserved.").arg(alias())); description->setWordWrap(true); layout->addWidget(description);
    auto *source = new QLineEdit; source->setObjectName("machineSetupSource"); source->setPlaceholderText(tr("Default: ~/.local/src/hgs on this device")); layout->addWidget(source);
    auto *browse = new QPushButton(tr("Choose HGS source folder…")); layout->addWidget(browse);
    connect(browse, &QPushButton::clicked, &dialog, [&] { const auto dir = QFileDialog::getExistingDirectory(&dialog, tr("HGS source folder")); if (!dir.isEmpty()) source->setText(dir); });
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); buttons->button(QDialogButtonBox::Ok)->setText(tr("Install HGS")); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    m_installing = true; m_log->clear(); m_log->show(); report(tr("Installing on %1… Keep Zerus running until setup finishes.").arg(alias())); updateControls(); m_client->setupMachine(alias(), source->text().trimmed());
}
