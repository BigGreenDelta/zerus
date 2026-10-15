#include "AutomaticApprovalSettings.h"
#include "AutomaticApprovals.h"
#include <QCheckBox>
#include <QDateTime>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {
QString status(const QString &value) {
    if (value == "approved") return QObject::tr("Approved once");
    if (value == "failed") return QObject::tr("Failed");
    if (value == "cancelled") return QObject::tr("Cancelled");
    return QObject::tr("Unconfirmed");
}
}

AutomaticApprovalSettings::AutomaticApprovalSettings(const QString &executable, QWidget *parent)
    : QWidget(parent), m_client(executable, this)
{
    setObjectName("automaticApprovalSettings"); setMaximumWidth(1100);
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(0, 0, 12, 0); layout->setSpacing(14);
    auto *title = new QLabel(tr("Automatic approvals")); title->setObjectName("heading"); layout->addWidget(title);
    m_enabled = new QCheckBox(tr("Automatically approve tool requests (dangerous)")); m_enabled->setObjectName("automaticApprovalsEnabled");
    m_enabled->setChecked(AutomaticApprovalPolicy::enabled()); layout->addWidget(m_enabled);
    auto *warning = new QLabel(tr("Allows agents to run commands and change files without waiting for you, including destructive actions and paid usage. Resuming after an expired cache may process the full conversation again. Applies to all reachable sessions while this Zerus is running, including with its window hidden. Ordinary questions, folder trust and permanent permission changes still need you."));
    warning->setObjectName("automaticApprovalWarning"); warning->setWordWrap(true); layout->addWidget(warning);
    auto *help = new QLabel(tr("Off by default and separate from automatic recovery. Supports verified one-time Claude, OpenCode and DeepSeek tool permissions. A journal entry must be saved first. Unconfirmed delivery is never retried automatically. Turning this off stops new approvals; requests already sent may still finish."));
    help->setWordWrap(true); layout->addWidget(help);
    connect(m_enabled, &QCheckBox::toggled, this, [this](bool enabled) {
        if (enabled) {
            QMessageBox confirm(QMessageBox::Warning, tr("Enable automatic approvals?"),
                tr("Agents on this computer and connected machines will be allowed to run tools without your review, including commands that delete files or consume paid usage. Resuming after an expired cache may process the full conversation again. Zerus will grant one-time permissions and record each attempt in this computer's private journal. Enable this dangerous mode?"), QMessageBox::NoButton, this);
            confirm.setObjectName("confirmAutomaticApprovals");
            auto *enable = confirm.addButton(tr("Enable automatic approvals"), QMessageBox::AcceptRole);
            auto *cancel = confirm.addButton(QMessageBox::Cancel); confirm.setDefaultButton(cancel); confirm.setEscapeButton(cancel);
            confirm.exec(); if (confirm.clickedButton() != enable) { const QSignalBlocker block(m_enabled); m_enabled->setChecked(false); return; }
        }
        AutomaticApprovalPolicy::setEnabled(enabled);
    });
    auto *journalTitle = new QLabel(tr("Approval journal")); journalTitle->setObjectName("heading"); layout->addWidget(journalTitle);
    auto *filters = new QHBoxLayout;
    m_host = new QLineEdit; m_host->setObjectName("approvalJournalHost"); m_host->setPlaceholderText(tr("Exact computer name (optional)"));
    m_session = new QLineEdit; m_session->setObjectName("approvalJournalSession"); m_session->setPlaceholderText(tr("Exact session name (optional)"));
    auto *reload = new QPushButton(tr("Refresh")); reload->setObjectName("refreshApprovalJournal");
    filters->addWidget(m_host); filters->addWidget(m_session, 1); filters->addWidget(reload); layout->addLayout(filters);
    m_table = new QTableWidget(0, 5); m_table->setObjectName("approvalJournal");
    m_table->setHorizontalHeaderLabels({tr("Time"),tr("Computer"),tr("Session"),tr("Question"),tr("Result")});
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers); m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection); m_table->setWordWrap(false); m_table->verticalHeader()->hide();
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive); m_table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_table->setColumnWidth(0,145); m_table->setColumnWidth(1,100); m_table->setColumnWidth(2,200); m_table->setColumnWidth(4,130);
    m_table->setMinimumHeight(180); layout->addWidget(m_table, 1);
    auto *navigation = new QHBoxLayout;
    m_newer = new QPushButton(tr("Newer")); m_newer->setObjectName("newerApprovalJournal");
    m_older = new QPushButton(tr("Older")); m_older->setObjectName("olderApprovalJournal");
    navigation->addWidget(m_newer); navigation->addWidget(m_older); navigation->addStretch();
    navigation->addWidget(new QLabel(tr("Up to 50 entries per page"))); layout->addLayout(navigation);
    m_detail = new QPlainTextEdit; m_detail->setObjectName("approvalJournalDetail"); m_detail->setReadOnly(true);
    m_detail->setPlaceholderText(tr("Select a journal entry to read the question and its outcome.")); m_detail->setMinimumHeight(150); layout->addWidget(m_detail, 1);
    m_status = new QLabel; m_status->setObjectName("approvalJournalStatus"); m_status->setWordWrap(true); m_status->setTextFormat(Qt::PlainText); layout->addWidget(m_status);
    auto *debounce = new QTimer(this); debounce->setSingleShot(true); debounce->setInterval(300);
    const auto firstPage = [this] { m_before = m_after = 0; refresh(); };
    connect(debounce, &QTimer::timeout, this, firstPage);
    connect(m_host, &QLineEdit::textChanged, debounce, qOverload<>(&QTimer::start));
    connect(m_session, &QLineEdit::textChanged, debounce, qOverload<>(&QTimer::start));
    connect(reload, &QPushButton::clicked, this, firstPage);
    connect(m_newer, &QPushButton::clicked, this, [this] { m_after = m_previous; m_before = 0; refresh(); });
    connect(m_older, &QPushButton::clicked, this, [this] { m_before = m_next; m_after = 0; refresh(); });
    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this] {
        const int row = m_table->currentRow(); if (row < 0 || !m_table->item(row, 0)) return;
        m_detail->clear(); m_detail->setPlaceholderText(tr("Loading question…"));
        m_detailRequest = m_client.requestApprovalLog("detail", {{"id",m_table->item(row,0)->data(Qt::UserRole).toLongLong()}});
    });
    connect(&m_client, &HgsClient::approvalLogFinished, this,
        [this](quint64 id, const QString &, const QJsonObject &result, const QString &error) {
            if (id == m_detailRequest) {
                if (!error.isEmpty()) { m_detail->setPlainText(error); return; }
                QStringList text{status(result.value("status").toString())};
                const auto answers = result.value("answers").toArray();
                for (const auto &value : result.value("question").toObject().value("questions").toArray()) {
                    const auto item = value.toObject(); text << item.value("question").toString() << item.value("body").toString();
                    for (const auto &option : item.value("options").toArray()) for (const auto &answer : answers)
                        if (answer.toObject().value("selected_option_ids").toArray().contains(option.toObject().value("id")))
                            text << tr("Selected answer: %1").arg(option.toObject().value("label").toString());
                }
                if (!result.value("detail").toString().isEmpty()) text << result.value("detail").toString();
                m_detail->setPlainText(text.join("\n\n")); return;
            }
            if (id != m_listRequest) return;
            m_listBusy = false;
            if (m_refreshAgain) { m_refreshAgain = false; refresh(); return; }
            m_enabled->setEnabled(error.isEmpty() || m_enabled->isChecked());
            if (!error.isEmpty()) { m_status->setText(error); return; }
            const QSignalBlocker block(m_table); const auto entries = result.value("entries").toArray();
            if (entries.size() > 50) { m_status->setText(tr("Invalid approval journal page.")); return; }
            m_table->setRowCount(0); m_table->setRowCount(entries.size());
            int row = 0;
            for (const auto &value : entries) {
                const auto entry = value.toObject();
                const QStringList values{QLocale().toString(QDateTime::fromMSecsSinceEpoch(entry.value("at").toInteger()),QLocale::ShortFormat),entry.value("host").toString(),entry.value("session").toString(),entry.value("prompt").toString(),status(entry.value("status").toString())};
                for (int column = 0; column < values.size(); ++column) {
                    auto *item = new QTableWidgetItem(values[column]); item->setToolTip(values[column]); item->setData(Qt::UserRole,entry.value("id").toInteger()); m_table->setItem(row,column,item);
                }
                ++row;
            }
            m_next = result.value("next").toInteger(); m_previous = result.value("previous").toInteger();
            m_table->setEnabled(true);
            m_newer->setEnabled(m_previous > 0); m_older->setEnabled(m_next > 0);
            m_status->setText(AutomaticApprovalPolicy::error().isEmpty() ? (entries.isEmpty() ? tr("No automatic approvals recorded for this filter.") : tr("%1 entries. Unconfirmed attempts require checking Activity or Terminal; they are not retried.").arg(entries.size())) : AutomaticApprovalPolicy::error());
        });
}

void AutomaticApprovalSettings::refresh()
{
    m_status->setText(tr("Loading approval journal…")); m_newer->setEnabled(false); m_older->setEnabled(false);
    m_detailRequest = 0; m_detail->clear(); m_detail->setPlaceholderText(tr("Select a journal entry to read the question and its outcome."));
    m_table->setEnabled(false);
    // Coalesce filter edits while a read is running; never build a queue of
    // obsolete pages when the local journal is busy.
    if (m_listBusy) { m_refreshAgain = true; return; }
    m_listBusy = true;
    QJsonObject request{{"host",m_host->text().trimmed()},{"session",m_session->text().trimmed()}};
    if (m_before > 0) request.insert("before",m_before); if (m_after > 0) request.insert("after",m_after);
    m_listRequest = m_client.requestApprovalLog("list",request);
}
