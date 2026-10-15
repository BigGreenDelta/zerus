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
#include <QTimer>

// Entries are immutable host/common-dir/path/fingerprint snapshots. Every native
// removal rechecks the snapshot, and uncertain failures are never replayed.
class WorktreeBatchCleanupDialog : public QDialog {
public:
    WorktreeBatchCleanupDialog(HgsClient *client, const QList<QJsonObject> &entries, QWidget *parent = nullptr)
        : QDialog(parent), m_client(client), m_entries(entries) {
        setObjectName("worktreeBatchCleanupDialog"); setWindowTitle(tr("Review cleanup")); resize(880, 600); setMinimumSize(640, 460);
        auto *layout = new QVBoxLayout(this); layout->setContentsMargins(20,18,20,18); layout->setSpacing(10);
        auto *heading = new QLabel(tr("Review %1 selected worktrees").arg(entries.size())); heading->setObjectName("heading"); layout->addWidget(heading);
        auto *hint = new QLabel(tr("Each computer checks these folders now. Folders still in use stay in place; other work can be removed after reviewing the files below. Branches are kept by default."));
        hint->setWordWrap(true); layout->addWidget(hint);
        m_list = new QTreeWidget; m_list->setObjectName("batchCleanupList"); m_list->setHeaderLabels({tr("Folder / branch"), tr("Computer"), tr("Size"), tr("Result")});
        m_list->setRootIsDecorated(false); m_list->header()->setSectionResizeMode(0, QHeaderView::Stretch); layout->addWidget(m_list, 1);
        m_details = new QPlainTextEdit; m_details->setReadOnly(true); m_details->setMaximumHeight(140); layout->addWidget(m_details);
        for (const auto &entry : entries) {
            const auto row = entry["row"].toObject();
            auto *item = new QTreeWidgetItem(m_list, {QFileInfo(row["path"].toString()).fileName() + " · " + row["branch"].toString(), entry["machine"].toString(), WorktreeReviewUi::size(row), tr("Checking…")});
            item->setData(0, Qt::UserRole, entry); item->setToolTip(0, row["path"].toString());
        }
        m_local = new QCheckBox(tr("Delete the listed local files outside Git too")); m_local->setObjectName("confirmIgnoredWorktreeFiles");
        m_local->hide(); layout->addWidget(m_local);
        m_discard = new QCheckBox(tr("Permanently discard uncommitted and untracked files")); m_discard->setObjectName("confirmDiscardWorktreeChanges"); m_discard->hide(); layout->addWidget(m_discard);
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
        connect(m_discard, &QCheckBox::toggled, this, [this] { updateAction(); });
        connect(m_list, &QTreeWidget::currentItemChanged, this, [this] { details(); });
        connect(m_remove, &QPushButton::clicked, this, [this] {
            if (!m_remove->isEnabled()) return;
            m_started = true; m_local->setEnabled(false); m_discard->setEnabled(false); m_branches->setEnabled(false); m_cancel->setText(tr("Stop after this folder")); updateAction(); next();
        });
        connect(client, &HgsClient::worktreeRemoved, this, [this](quint64 request, bool ok, const QJsonObject &result, const QString &error) {
            if (request != m_request) return; m_request = 0;
            auto *item = m_list->topLevelItem(m_index); item->setText(3, ok ? (result["status"] == "forgotten" ? tr("Forgotten") : tr("Removed")) : tr("Not confirmed"));
            item->setToolTip(3, ok ? result["branch_error"].toString() : error);
            if (ok) { m_changed = true; ++m_removed; } else ++m_failed;
            if (!error.isEmpty()) m_errors << item->text(0) + ": " + error;
            if (!result["branch_error"].toString().isEmpty()) m_errors << result["branch_error"].toString();
            ++m_index; next();
        });
        connect(client, &HgsClient::worktreeReviewReady, this, [this](quint64 request, const QString &host, const QString &path, const QJsonObject &data) {
            if (request != m_reviewRequest) return;
            auto &entry = m_entries[m_reviewIndex]; const auto target = entry["row"].toObject()["path"].toString();
            QJsonObject verified; int matches = 0;
            if (host == entry["host"].toString() && path == entry["common_dir"].toString() && data["state"] == "ok" && data["common_dir"] == entry["common_dir"])
                for (const auto &value : data["worktrees"].toArray()) if (value.toObject()["path"] == target) { verified = value.toObject(); ++matches; }
            if (matches == 1) { entry["row"] = verified; entry["verified"] = true; }
            else entry["review_error"] = tr("Could not verify this folder. Refresh its catalog and try again.");
            finishReview();
        });
        connect(client, &HgsClient::worktreeReviewFailed, this, [this](quint64 request, const QString &, const QString &, const QString &error) {
            if (request != m_reviewRequest) return; m_entries[m_reviewIndex]["review_error"] = error; finishReview();
        });
        if (m_list->topLevelItemCount()) m_list->setCurrentItem(m_list->topLevelItem(0));
        updateAction(); QTimer::singleShot(0, this, [this] { reviewNext(); });
    }
    bool changed() const { return m_changed; }
    bool attempted() const { return m_started; }
    QString summary() const { return tr("%1 removed · %2 unconfirmed").arg(m_removed).arg(m_failed) + (m_skipped ? tr(" · %1 kept").arg(m_skipped) : QString()); }
    void reject() override { if (!m_request && (!m_started || m_index == m_entries.size())) QDialog::reject(); }
private:
    bool eligible(const QJsonObject &entry) const { return entry["verified"] == true && (WorktreeReviewUi::removable(entry["row"].toObject()) || discardable(entry["row"].toObject())); }
    static bool discardable(const QJsonObject &row) { return row["discardable"] == true && !row["discard_fingerprint"].toString().isEmpty(); }
    void updateAction() {
        int count = 0; m_requiresLocal = false; m_requiresDiscard = false;
        for (const auto &entry : m_entries) if (eligible(entry)) { ++count; const auto row=entry["row"].toObject(); m_requiresLocal |= WorktreeReviewUi::localData(row); m_requiresDiscard |= discardable(row); }
        m_local->setVisible(m_requiresLocal); m_discard->setVisible(m_requiresDiscard);
        m_remove->setText(tr("Remove %1 worktrees").arg(count));
        m_remove->setEnabled(!m_started && m_reviewIndex == m_entries.size() && count > 0 && (!m_requiresLocal || m_local->isChecked()) && (!m_requiresDiscard || m_discard->isChecked()));
    }
    void reviewNext() {
        while (m_reviewIndex < m_entries.size()) {
            auto &entry = m_entries[m_reviewIndex];
            if (entry["offline"] == true || entry["common_dir"].toString().isEmpty()) {
                entry["review_error"] = tr("Reconnect this computer and refresh to check the folder."); finishReview(); return;
            }
            m_status->setText(tr("Checking %1 of %2…").arg(m_reviewIndex + 1).arg(m_entries.size()));
            m_reviewRequest = m_client->reviewWorktrees(entry["host"].toString(), entry["common_dir"].toString(), entry["row"].toObject()["path"].toString()); return;
        }
        m_status->setText(tr("Review the files and warnings, then confirm. Folders marked Kept will not be removed.")); updateAction(); details();
    }
    void finishReview() {
        m_reviewRequest = 0; const auto entry = m_entries[m_reviewIndex], row = entry["row"].toObject(); auto *item = m_list->topLevelItem(m_reviewIndex);
        item->setText(0, QFileInfo(row["path"].toString()).fileName() + " · " + row["branch"].toString());
        item->setData(0, Qt::UserRole, entry); item->setText(2, WorktreeReviewUi::size(row));
        item->setText(3, eligible(entry) ? (discardable(row) ? tr("Confirm discard") : tr("Ready for confirmation")) : tr("Kept"));
        item->setToolTip(3, entry["review_error"].toString(WorktreeReviewUi::summary(row))); ++m_reviewIndex; details(); reviewNext();
    }
    void details() {
        const auto *item = m_list->currentItem(); if (!item) return;
        const auto entry = item->data(0, Qt::UserRole).toJsonObject(), row = entry["row"].toObject();
        QStringList lines{entry["machine"].toString() + ": " + row["path"].toString(), WorktreeReviewUi::summary(row)};
        if (!entry["review_error"].toString().isEmpty()) lines << entry["review_error"].toString();
        for (const auto &value : row["changed_files"].toArray()) lines << tr("Discard: %1").arg(value.toString());
        for (const auto &value : row["notes"].toArray()) lines << value.toObject()["message"].toString();
        for (const auto &value : row["ignored"].toArray()) { const auto file = value.toObject(); lines << tr("Delete local data: %1 (%2)").arg(file["path"].toString(), QLocale().formattedDataSize(qint64(file["bytes"].toDouble()))); }
        if (row["ignored_count"].toInt() > row["ignored"].toArray().size()) lines << tr("Additional local entries are present; inspect them before confirming.");
        lines << (row["merged"] == true ? tr("Branch may be deleted only if you choose it below.") : tr("Original branch will be kept."));
        m_details->setPlainText(lines.join('\n'));
    }
    void next() {
        if (m_stop) while (m_index < m_entries.size()) { m_list->topLevelItem(m_index++)->setText(3, tr("Kept; not attempted")); ++m_skipped; }
        while (m_index < m_entries.size() && !eligible(m_entries[m_index])) { ++m_skipped; ++m_index; }
        if (m_index == m_entries.size()) {
            m_cancel->setText(tr("Close")); m_cancel->setEnabled(true);
            m_status->setText(summary() + (m_skipped ? tr(" without an attempt") : QString()) + (m_errors.isEmpty() ? QString() : '\n' + m_errors.join('\n')));
            if (!m_failed && !m_skipped && m_errors.isEmpty()) accept(); return;
        }
        const auto entry = m_entries[m_index], row = entry["row"].toObject();
        m_list->topLevelItem(m_index)->setText(3, tr("Checking and removing…"));
        m_status->setText(tr("%1 of %2 · %3").arg(m_index + 1).arg(m_entries.size()).arg(entry["machine"].toString()));
        m_request = m_client->removeWorktree(entry["host"].toString(), row["path"].toString(), entry["common_dir"].toString(), row["fingerprint"].toString(),
            m_branches->isChecked() && row["merged"] == true, m_requiresLocal && m_local->isChecked() ? row["ignored_fingerprint"].toString() : QString(),
            m_requiresDiscard && m_discard->isChecked() ? row["discard_fingerprint"].toString() : QString());
    }
    HgsClient *m_client; QList<QJsonObject> m_entries; QTreeWidget *m_list; QPlainTextEdit *m_details;
    QCheckBox *m_local, *m_discard, *m_branches; QLabel *m_status; QPushButton *m_cancel, *m_remove;
    QStringList m_errors; quint64 m_request = 0, m_reviewRequest = 0; int m_reviewIndex = 0, m_index = 0, m_removed = 0, m_failed = 0, m_skipped = 0;
    bool m_started = false, m_changed = false, m_requiresLocal = false, m_requiresDiscard = false, m_stop = false;
};
