#pragma once
#include "HgsClient.h"
#include "WorktreeReviewUi.h"
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

// Entries are immutable host/common-dir/path/fingerprint snapshots. Every native
// removal rechecks the snapshot, and uncertain failures are never replayed.
class WorktreeBatchCleanupDialog : public QDialog {
public:
    WorktreeBatchCleanupDialog(HgsClient *client, const QList<QJsonObject> &entries, QWidget *parent = nullptr)
        : QDialog(parent), m_client(client), m_entries(entries) {
        setObjectName("worktreeBatchCleanupDialog"); setWindowTitle(tr("Review cleanup")); resize(880, 600); setMinimumSize(640, 460);
        auto *layout = new QVBoxLayout(this);
        auto *heading = new QLabel(tr("Remove %1 selected worktrees?").arg(entries.size())); heading->setObjectName("heading"); layout->addWidget(heading);
        auto *hint = new QLabel(tr("Review the exact folders below. Local files are deleted with their folder. Each computer checks the worktree again before removal."));
        hint->setWordWrap(true); layout->addWidget(hint);
        m_list = new QTreeWidget; m_list->setObjectName("batchCleanupList"); m_list->setHeaderLabels({tr("Folder / branch"), tr("Computer"), tr("Size"), tr("Result")});
        m_list->setRootIsDecorated(false); m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch); layout->addWidget(m_list, 1);
        m_details = new QPlainTextEdit; m_details->setReadOnly(true); m_details->setMaximumHeight(140); layout->addWidget(m_details);
        bool ignored = false; qint64 bytes = 0; bool complete = true;
        for (const auto &entry : entries) {
            const auto row = entry["row"].toObject(); ignored |= WorktreeReviewUi::localData(row);
            bytes += qint64(row["disk_bytes"].toDouble()); complete &= row["verdict"] == "missing" || row["disk_complete"] == true;
            auto *item = new QTreeWidgetItem(m_list, {QFileInfo(row["path"].toString()).fileName() + " · " + row["branch"].toString(), entry["machine"].toString(), WorktreeReviewUi::size(row), tr("Waiting for confirmation")});
            item->setData(0, Qt::UserRole, entry); item->setToolTip(0, row["path"].toString());
        }
        auto *total = new QLabel(tr("Estimated space: %1%2. Branches are kept unless selected below.").arg(complete ? QString() : tr("at least "), QLocale().formattedDataSize(bytes)));
        total->setWordWrap(true); layout->addWidget(total);
        m_local = new QCheckBox(tr("I reviewed the local files outside Git and want to delete them too")); m_local->setObjectName("confirmIgnoredWorktreeFiles");
        m_local->setVisible(ignored); m_requiresLocal = ignored; layout->addWidget(m_local);
        m_branches = new QCheckBox(tr("Also delete branches proven merged by Git")); m_branches->setObjectName("batchDeleteMergedBranches"); layout->addWidget(m_branches);
        m_status = new QLabel(tr("This removal cannot be undone.")); m_status->setTextFormat(Qt::PlainText);m_status->setObjectName("batchCleanupStatus"); m_status->setWordWrap(true); layout->addWidget(m_status);
        auto *buttons = new QDialogButtonBox;
        m_cancel = buttons->addButton(QDialogButtonBox::Cancel); m_remove = buttons->addButton(tr("Remove %1 worktrees").arg(entries.size()), QDialogButtonBox::DestructiveRole);
        m_remove->setObjectName("confirmBatchWorktreeRemoval"); m_remove->setAutoDefault(false); m_cancel->setDefault(true); layout->addWidget(buttons);
        connect(m_cancel, &QPushButton::clicked, this, [this] {
            if (m_request) { m_stop = true; m_cancel->setEnabled(false); m_status->setText(tr("Finishing the current folder; remaining folders will be kept.")); }
            else reject();
        });
        connect(m_local, &QCheckBox::toggled, this, [this] { updateAction(); });
        connect(m_list, &QTreeWidget::currentItemChanged, this, [this] { details(); });
        connect(m_remove, &QPushButton::clicked, this, [this] {
            if (m_started || (m_requiresLocal && !m_local->isChecked())) return;
            m_started = true; m_local->setEnabled(false); m_branches->setEnabled(false); m_cancel->setText(tr("Stop after this folder")); updateAction(); next();
        });
        connect(client, &HgsClient::worktreeRemoved, this, [this](quint64 request, bool ok, const QJsonObject &result, const QString &error) {
            if (request != m_request) return; m_request = 0;
            auto *item = m_list->topLevelItem(m_index); item->setText(3, ok ? (result["status"] == "forgotten" ? tr("Forgotten") : tr("Removed")) : tr("Not removed"));
            item->setToolTip(3, ok ? result["branch_error"].toString() : error);
            if (ok) { m_changed = true; ++m_removed; } else ++m_failed;
            if (!error.isEmpty()) m_errors << item->text(0) + ": " + error;
            if (!result["branch_error"].toString().isEmpty()) m_errors << result["branch_error"].toString();
            ++m_index; next();
        });
        if (m_list->topLevelItemCount()) m_list->setCurrentItem(m_list->topLevelItem(0));
        updateAction();
    }
    bool changed() const { return m_changed; }
    bool attempted() const { return m_started; }
    void reject() override { if (!m_request && (!m_started || m_index == m_entries.size())) QDialog::reject(); }
private:
    void updateAction() { m_remove->setEnabled(!m_started && !m_entries.isEmpty() && (!m_requiresLocal || m_local->isChecked())); }
    void details() {
        const auto *item = m_list->currentItem(); if (!item) return;
        const auto entry = item->data(0, Qt::UserRole).toJsonObject(), row = entry["row"].toObject();
        QStringList lines{entry["machine"].toString() + ": " + row["path"].toString(), WorktreeReviewUi::summary(row)};
        for (const auto &value : row["ignored"].toArray()) { const auto file = value.toObject(); lines << tr("Delete local data: %1 (%2)").arg(file["path"].toString(), QLocale().formattedDataSize(qint64(file["bytes"].toDouble()))); }
        if (row["ignored_count"].toInt() > row["ignored"].toArray().size()) lines << tr("Additional local entries are present; inspect them before confirming.");
        lines << (row["merged"] == true ? tr("Branch may be deleted only if you choose it below.") : tr("Original branch will be kept."));
        m_details->setPlainText(lines.join('\n'));
    }
    void next() {
        if (m_stop) while (m_index < m_entries.size()) { m_list->topLevelItem(m_index++)->setText(3, tr("Kept; not attempted")); ++m_skipped; }
        if (m_index == m_entries.size()) {
            m_cancel->setText(tr("Close")); m_cancel->setEnabled(true);
            m_status->setText(tr("%1 removed · %2 not removed").arg(m_removed).arg(m_failed) + (m_skipped ? tr(" · %1 kept without an attempt").arg(m_skipped) : QString()) + (m_errors.isEmpty() ? QString() : '\n' + m_errors.join('\n'))); return;
        }
        const auto entry = m_entries[m_index], row = entry["row"].toObject();
        m_list->topLevelItem(m_index)->setText(3, tr("Checking and removing…"));
        m_status->setText(tr("%1 of %2 · %3").arg(m_index + 1).arg(m_entries.size()).arg(entry["machine"].toString()));
        m_request = m_client->removeWorktree(entry["host"].toString(), row["path"].toString(), entry["common_dir"].toString(), row["fingerprint"].toString(),
            m_branches->isChecked() && row["merged"] == true, m_requiresLocal && m_local->isChecked() ? row["ignored_fingerprint"].toString() : QString());
    }
    HgsClient *m_client; QList<QJsonObject> m_entries; QTreeWidget *m_list; QPlainTextEdit *m_details;
    QCheckBox *m_local, *m_branches; QLabel *m_status; QPushButton *m_cancel, *m_remove;
    QStringList m_errors; quint64 m_request = 0; int m_index = 0, m_removed = 0, m_failed = 0, m_skipped = 0;
    bool m_started = false, m_changed = false, m_requiresLocal = false, m_stop = false;
};
