#include "PhonePairing.h"
#include "HgsClient.h"
#include "qrcodegen.hpp"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QFontDatabase>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <utility>

namespace PhonePairing {
namespace {
constexpr int QuietZone = 4;
enum Role { KindRole = Qt::UserRole, IdRole, HostRole, WorkspaceRole, TargetRole, WorkspaceIdRole, ServerRole };
QString hostName(const QString &host) { return host.isEmpty() ? QCoreApplication::translate("PhonePairing", "this computer") : host; }
QString when(const QJsonValue &seconds) {
    if (!seconds.isDouble() || seconds.toDouble() <= 0) return QStringLiteral("—");
    return QLocale().toString(QDateTime::fromMSecsSinceEpoch(qint64(seconds.toDouble() * 1000)), QLocale::ShortFormat);
}
QString origin(QString url) { while (url.endsWith('/')) url.chop(1); return url; }
// Pins the relay and workspace the user saw, so a gateway whose connection
// changed meanwhile neither issues nor revokes for another workspace.
QStringList pinned(const QString &action, const QString &id, const QString &workspace, const QString &server) {
    QStringList args{action};
    if (!id.isEmpty()) args << id;
    if (!workspace.isEmpty() && !server.isEmpty()) args << QStringLiteral("--workspace") << workspace << QStringLiteral("--server") << server;
    return args << QStringLiteral("--json");
}
QString platform(const QString &value) {
    if (value == QLatin1String("ios")) return QStringLiteral("iOS");
    if (value == QLatin1String("android")) return QStringLiteral("Android");
    return QStringLiteral("—");
}
} // namespace

std::vector<bool> encode(const QString &text, int *size) {
    *size = 0;
    const auto utf8 = text.toUtf8();
    if (utf8.isEmpty() || utf8.contains('\0')) return {};
    try {
        const auto qr = qrcodegen::QrCode::encodeText(utf8.constData(), qrcodegen::QrCode::Ecc::MEDIUM);
        const int n = qr.getSize();
        std::vector<bool> modules(size_t(n) * size_t(n));
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) modules[size_t(y) * size_t(n) + size_t(x)] = qr.getModule(x, y);
        *size = n;
        return modules;
    } catch (const std::exception &) {
        return {};
    }
}

QrCodeView::QrCodeView(QWidget *parent) : QWidget(parent) {
    setObjectName("phonePairingCode"); setMinimumSize(200, 200);
    setAccessibleName(tr("Pairing QR code"));
}
void QrCodeView::setText(const QString &text) { m_text = text; m_modules = encode(text, &m_size); update(); }
void QrCodeView::setFaded(bool faded) { m_faded = faded; update(); }
void QrCodeView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    const int side = qMin(width(), height());
    const QRect square((width() - side) / 2, (height() - side) / 2, side, side);
    // Dark modules on white in every theme: phone cameras need the contrast.
    p.fillRect(square, Qt::white);
    if (!m_size) return;
    const int cell = side / (m_size + 2 * QuietZone);
    if (cell < 1) return;
    const int origin = (side - cell * m_size) / 2;
    if (m_faded) p.setOpacity(0.15);
    for (int y = 0; y < m_size; ++y)
        for (int x = 0; x < m_size; ++x)
            if (m_modules[size_t(y) * size_t(m_size) + size_t(x)])
                p.fillRect(square.x() + origin + x * cell, square.y() + origin + y * cell, cell, cell, Qt::black);
}

Dialog::Dialog(const QString &executable, const QList<Target> &targets, int selected, QWidget *parent, std::function<double()> now)
    : QDialog(parent), m_client(new HgsClient(executable, this)), m_targets(targets), m_now(std::move(now)) {
    if (!m_now) m_now = [] { return QDateTime::currentMSecsSinceEpoch() / 1000.0; };
    setObjectName("phonePairingDialog"); setWindowTitle(tr("Pair a phone"));
    auto *layout = new QVBoxLayout(this); layout->setSpacing(12);
    if (m_targets.size() > 1) {
        m_workspace = new QComboBox; m_workspace->setObjectName("phonePairingWorkspace"); m_workspace->setAccessibleName(tr("Workspace"));
        for (const auto &target : m_targets) m_workspace->addItem(tr("%1 · via %2").arg(target.workspaceName, hostName(target.host)));
        m_workspace->setCurrentIndex(qBound(0, selected, int(m_targets.size()) - 1));
        auto *row = new QFormLayout; row->addRow(tr("Workspace"), m_workspace); layout->addLayout(row);
        connect(m_workspace, &QComboBox::currentIndexChanged, this, [this] { request(); });
    }
    auto *hint = new QLabel(tr("In Zerus on the phone, open Machines → Pair another workspace and scan this code. "
                               "A phone that is not paired yet offers pairing when it opens. "
                               "The code works once, for ten minutes; treat it like a password."));
    hint->setWordWrap(true); layout->addWidget(hint);
    m_code = new QrCodeView; layout->addWidget(m_code, 1, Qt::AlignHCenter);
    auto *form = new QFormLayout;
    m_server = new QLabel; m_server->setObjectName("phonePairingServer");
    m_pairCode = new QLabel; m_pairCode->setObjectName("phonePairingCodeText");
    m_pairCode->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    for (auto *label : {m_server, m_pairCode}) { label->setTextFormat(Qt::PlainText); label->setTextInteractionFlags(Qt::TextSelectableByMouse); label->setWordWrap(true); }
    form->addRow(tr("Server"), m_server); form->addRow(tr("Code"), m_pairCode); layout->addLayout(form);
    m_expiry = new QLabel; m_expiry->setObjectName("phonePairingExpiry"); layout->addWidget(m_expiry);
    m_status = new QLabel; m_status->setObjectName("phonePairingStatus"); m_status->setWordWrap(true); m_status->setTextFormat(Qt::PlainText);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(m_status);
    auto *buttons = new QHBoxLayout;
    m_copyCode = new QPushButton(tr("Copy code")); m_copyCode->setObjectName("copyPairingCode");
    m_copyLink = new QPushButton(tr("Copy link")); m_copyLink->setObjectName("copyPairingLink");
    m_again = new QPushButton(tr("New code")); m_again->setObjectName("newPairingCode");
    auto *close = new QPushButton(tr("Close")); close->setObjectName("closePhonePairing");
    buttons->addWidget(m_copyCode); buttons->addWidget(m_copyLink); buttons->addWidget(m_again); buttons->addStretch(); buttons->addWidget(close);
    layout->addLayout(buttons);
    connect(m_copyCode, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(m_codeText); });
    connect(m_copyLink, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(m_link); });
    connect(m_again, &QPushButton::clicked, this, [this] { request(); });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    m_timer = new QTimer(this); m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, [this] { tick(); });
    connect(m_client, &HgsClient::mobileReady, this, [this](quint64 request, const QString &, const QJsonObject &result) {
        if (request != m_request) return;
        m_request = 0; showInvitation(result);
    });
    connect(m_client, &HgsClient::mobileFailed, this, [this](quint64 request, const QString &, const QString &error) {
        if (request != m_request) return;
        m_request = 0; m_status->setText(error); m_again->setEnabled(true);
    });
    // The invitation stays a credential until it is used or expires.
    connect(this, &QDialog::finished, this, [this] { m_link.clear(); m_codeText.clear(); m_code->setText({}); m_timer->stop(); });
    request();
}

void Dialog::request() {
    if (m_targets.isEmpty()) return;
    const auto target = m_current = m_targets.value(m_workspace ? m_workspace->currentIndex() : 0);
    m_link.clear(); m_codeText.clear(); m_expires = 0; m_timer->stop();
    m_code->setText({}); m_code->setFaded(false);
    m_server->setText(target.serverUrl); m_pairCode->setText(QStringLiteral("…")); m_expiry->clear();
    m_status->setText(tr("Requesting a pairing code from %1…").arg(hostName(target.host)));
    m_copyCode->setEnabled(false); m_copyLink->setEnabled(false); m_again->setEnabled(false);
    m_request = m_client->requestMobile(target.host, pinned(QStringLiteral("invite"), {}, target.workspaceId, target.serverUrl));
}

void Dialog::showInvitation(const QJsonObject &invitation) {
    m_link = invitation.value("pair_url").toString(); m_codeText = invitation.value("pair_code").toString();
    m_expires = invitation.value("expires_at").toDouble();
    m_again->setEnabled(true);
    if (!m_link.startsWith(QLatin1String("zerus://pair?")) || m_codeText.isEmpty()) {
        m_link.clear(); m_codeText.clear(); m_status->setText(tr("The relay returned an invalid pairing code.")); return;
    }
    if (invitation.value("workspace_id").toString() != m_current.workspaceId
        || origin(invitation.value("server_url").toString()) != origin(m_current.serverUrl)) {
        m_link.clear(); m_codeText.clear();
        m_status->setText(tr("This pairing code belongs to another workspace or relay than the one listed. Refresh the phone list and try again."));
        return;
    }
    m_code->setText(m_link);
    m_server->setText(invitation.value("server_url").toString());
    m_pairCode->setText(m_codeText);
    m_status->setText(tr("Workspace: %1").arg(invitation.value("workspace_name").toString()));
    m_copyCode->setEnabled(true); m_copyLink->setEnabled(true);
    tick(); m_timer->start();
}

void Dialog::tick() {
    const double left = m_expires - m_now();
    if (left <= 0) {
        m_timer->stop(); m_code->setFaded(true); m_copyCode->setEnabled(false); m_copyLink->setEnabled(false);
        m_expiry->setText(tr("This code expired. Choose New code to pair."));
        return;
    }
    const int seconds = int(std::ceil(left));
    m_expiry->setText(tr("Expires in %1:%2").arg(seconds / 60).arg(seconds % 60, 2, 10, QLatin1Char('0')));
}

Panel::Panel(const QString &executable, QWidget *parent, std::function<bool(const QString &, const QString &)> confirm)
    : QWidget(parent), m_executable(executable), m_client(new HgsClient(executable, this)), m_confirm(std::move(confirm)) {
    setObjectName("phonesPanel");
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 12, 0); layout->setSpacing(10);
    auto *heading = new QLabel(tr("Phones")); heading->setObjectName("heading"); layout->addWidget(heading);
    auto *intro = new QLabel(tr("Phones paired with your computers. A phone can control every computer of its workspace; "
                                "revoke one you no longer use."));
    intro->setObjectName("muted"); intro->setWordWrap(true); layout->addWidget(intro);
    m_tree = new QTreeWidget; m_tree->setObjectName("linkedPhones"); m_tree->setAccessibleName(tr("Phones"));
    m_tree->setHeaderLabels({tr("Phone"), tr("Platform"), tr("Paired"), tr("Last active"), tr("Added from")});
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection); m_tree->setUniformRowHeights(true);
    m_tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents); m_tree->header()->setStretchLastSection(true);
    m_tree->setMinimumHeight(140); layout->addWidget(m_tree);
    auto *buttons = new QHBoxLayout;
    m_pair = new QPushButton(tr("Pair a phone…")); m_pair->setObjectName("pairPhone");
    m_revoke = new QPushButton(tr("Revoke…")); m_revoke->setObjectName("revokePhone");
    m_refresh = new QPushButton(tr("Refresh")); m_refresh->setObjectName("refreshPhones");
    buttons->addWidget(m_pair); buttons->addWidget(m_revoke); buttons->addWidget(m_refresh); buttons->addStretch(); layout->addLayout(buttons);
    m_status = new QLabel; m_status->setObjectName("phonesStatus"); m_status->setWordWrap(true); m_status->setTextFormat(Qt::PlainText);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(m_status);
    m_pair->setEnabled(false); m_revoke->setEnabled(false);
    connect(m_pair, &QPushButton::clicked, this, [this] { pair(); });
    connect(m_revoke, &QPushButton::clicked, this, [this] { revoke(); });
    connect(m_refresh, &QPushButton::clicked, this, [this] { m_notice.clear(); refresh(); });
    connect(m_tree, &QTreeWidget::itemSelectionChanged, this, [this] {
        const auto *item = m_tree->currentItem();
        m_revoke->setEnabled(!m_revoking && item && item->data(0, KindRole).toString() == QLatin1String("phone"));
    });
    connect(m_client, &HgsClient::machinesReady, this, [this](quint64 request, const QJsonObject &result) {
        if (request != m_machines) return;
        m_machines = 0;
        QStringList hosts{QString()};
        for (const auto &value : result.value("machines").toArray()) {
            const auto row = value.toObject();
            if (row.value("enabled").toBool() && !row.value("alias").toString().isEmpty()) hosts << row.value("alias").toString();
        }
        m_hostOrder = hosts;
        for (const auto &host : hosts) m_pending.insert(m_client->requestMobile(host, {QStringLiteral("phones"), QStringLiteral("--json")}));
    });
    connect(m_client, &HgsClient::machinesFailed, this, [this](quint64 request, const QString &error) {
        if (request != m_machines) return;
        m_machines = 0; m_problems << tr("Could not read the machine list: %1").arg(error);
        m_hostOrder = {QString()};
        m_pending.insert(m_client->requestMobile(QString(), {QStringLiteral("phones"), QStringLiteral("--json")}));
    });
    connect(m_client, &HgsClient::mobileReady, this, [this](quint64 request, const QString &host, const QJsonObject &result) {
        if (request && request == m_revoking) { m_revoking = 0; m_notice = tr("%1 revoked.").arg(m_revokingName); refresh(); return; }
        if (!m_pending.remove(request)) return;
        if (result.value("configured").toBool()) { auto list = result; list.insert("host", host); m_lists << list; }
        if (m_pending.isEmpty()) finish();
    });
    connect(m_client, &HgsClient::mobileFailed, this, [this](quint64 request, const QString &host, const QString &error) {
        if (request && request == m_revoking) {
            m_revoking = 0;
            m_notice = tr("Could not confirm revoking %1: %2. Refresh to check whether it is still listed before trying again.")
                           .arg(m_revokingName, error);
            render(); return;
        }
        if (!m_pending.remove(request)) return;
        m_problems << tr("%1: %2").arg(hostName(host), error);
        if (m_pending.isEmpty()) finish();
    });
}

void Panel::showEvent(QShowEvent *event) { QWidget::showEvent(event); if (!m_loaded) refresh(); }

void Panel::refresh() {
    if (m_machines || !m_pending.isEmpty()) return;
    m_loaded = true; m_lists.clear(); m_problems.clear();
    m_status->setText(tr("Checking phones…")); m_refresh->setEnabled(false);
    m_machines = m_client->requestMachines({QStringLiteral("ls")});
}

void Panel::finish() {
    // This computer first, then peers in Machines order; one route per workspace.
    std::stable_sort(m_lists.begin(), m_lists.end(), [this](const QJsonObject &a, const QJsonObject &b) {
        return m_hostOrder.indexOf(a.value("host").toString()) < m_hostOrder.indexOf(b.value("host").toString());
    });
    QList<QJsonObject> unique; QSet<QString> seen;
    for (const auto &list : std::as_const(m_lists)) {
        const auto id = list.value("workspace_id").toString();
        if (id.isEmpty() || seen.contains(id)) continue;
        seen.insert(id); unique << list;
    }
    m_lists = unique; m_targets.clear();
    for (const auto &list : std::as_const(m_lists))
        m_targets << Target{list.value("host").toString(), list.value("workspace_id").toString(),
                            list.value("workspace_name").toString(), list.value("server_url").toString()};
    render();
}

void Panel::render() {
    const auto selected = m_tree->currentItem() ? m_tree->currentItem()->data(0, IdRole).toString() : QString();
    m_tree->clear(); int phones = 0;
    for (int index = 0; index < m_lists.size(); ++index) {
        const auto &list = m_lists[index]; const auto host = list.value("host").toString();
        const auto workspace = list.value("workspace_name").toString();
        auto *top = new QTreeWidgetItem(m_tree, {host.isEmpty() ? tr("%1 · this computer").arg(workspace) : tr("%1 · via %2").arg(workspace, host)});
        top->setData(0, KindRole, "workspace"); top->setData(0, TargetRole, index); top->setFirstColumnSpanned(true);
        for (const auto &value : list.value("devices").toArray()) {
            const auto device = value.toObject(); const auto added = device.value("added_by").toObject();
            auto *item = new QTreeWidgetItem(top, {device.value("name").toString(), platform(device.value("platform").toString()),
                when(device.value("paired_at")), when(device.value("last_seen_at")),
                added.isEmpty() ? QStringLiteral("—") : added.value("name").toString()});
            if (added.isEmpty()) item->setToolTip(4, tr("Paired with an administrator invitation, or before Zerus recorded it"));
            item->setData(0, KindRole, "phone"); item->setData(0, IdRole, device.value("id").toString());
            item->setData(0, HostRole, host); item->setData(0, WorkspaceRole, workspace); item->setData(0, TargetRole, index);
            item->setData(0, WorkspaceIdRole, list.value("workspace_id").toString()); item->setData(0, ServerRole, list.value("server_url").toString());
            if (item->data(0, IdRole).toString() == selected) m_tree->setCurrentItem(item);
            ++phones;
        }
    }
    m_tree->expandAll();
    QStringList lines;
    if (!m_notice.isEmpty()) lines << m_notice;
    if (m_targets.isEmpty()) {
#ifdef Q_OS_WIN
        lines << tr("No mobile connection is configured. Open Settings → Mobile connection on this Windows PC, save its relay configuration and start the connector. Then refresh Phones.");
#else
        lines << tr("No computer here has a mobile connection. Set one up in Settings → Mobile connection on a computer with local sessions enabled, then refresh Phones.");
#endif
    }
    else lines << (phones == 1 ? tr("1 phone") : tr("%1 phones").arg(phones));
    for (const auto &problem : std::as_const(m_problems)) lines << problem;
    if (!m_revoking) m_status->setText(lines.join('\n'));
    m_pair->setEnabled(!m_targets.isEmpty()); m_refresh->setEnabled(true);
    const auto *item = m_tree->currentItem();
    m_revoke->setEnabled(!m_revoking && item && item->data(0, KindRole).toString() == QLatin1String("phone"));
}

void Panel::revoke() {
    const auto *item = m_tree->currentItem();
    if (m_revoking || !item || item->data(0, KindRole).toString() != QLatin1String("phone")) return;
    const auto phone = item->text(0), workspace = item->data(0, WorkspaceRole).toString();
    const auto id = item->data(0, IdRole).toString(), host = item->data(0, HostRole).toString();
    const auto workspaceId = item->data(0, WorkspaceIdRole).toString(), server = item->data(0, ServerRole).toString();
    const bool confirmed = m_confirm ? m_confirm(phone, workspace) : [&] {
        QMessageBox box(QMessageBox::Warning, tr("Revoke %1?").arg(phone),
            tr("%1 loses access to every computer of %2 at once, and its queued requests are cancelled. "
               "Pair it again to restore access.").arg(phone, workspace), QMessageBox::Cancel, this);
        auto *revoke = box.addButton(tr("Revoke"), QMessageBox::DestructiveRole);
        box.setDefaultButton(QMessageBox::Cancel); box.exec();
        return box.clickedButton() == revoke;
    }();
    if (!confirmed) return;
    m_notice.clear(); m_revokingName = phone;
    m_status->setText(tr("Revoking %1…").arg(phone)); m_revoke->setEnabled(false);
    m_revoking = m_client->requestMobile(host, pinned(QStringLiteral("revoke-phone"), id, workspaceId, server));
}

void Panel::pair() {
    if (m_targets.isEmpty()) return;
    const auto *item = m_tree->currentItem();
    Dialog dialog(m_executable, m_targets, item ? item->data(0, TargetRole).toInt() : 0, this);
    dialog.exec();
    m_notice.clear(); refresh();
}

} // namespace PhonePairing
