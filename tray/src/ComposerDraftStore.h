#pragma once

#include "HgsClient.h"
#include <functional>
#include <QDateTime>
#include <QHash>

class QObject;

struct ComposerDraft {
    QString text, notice, storageError, label;
    QJsonObject formState;
    QList<MessageAttachment> attachments;
    QStringList attachmentHashes;
    int nextAttachmentNumber = 1;
    int position = 0, anchor = 0;
    bool sending = false, error = false, uncertain = false;
};

// What Saved drafts lists for one stored draft. The label is empty unless the
// draft's owner named it.
struct ComposerDraftSummary {
    QString label, preview;
    QDateTime modified;
    int attachments = 0;
};

// The conversation a stored draft belongs to. Message, subagent, question-answer,
// voice and task-prompt drafts share one store and name their session differently.
struct ComposerDraftOwner {
    enum class Kind { Unknown, Message, Subagent, Question, Voice, Task };
    Kind kind = Kind::Unknown;
    // The host alias, empty for this computer; a subagent key holds the machine
    // name instead. Archive is set for an archived session.
    QString machine, session, archive, conversation, subagent;
};

// Private desktop state, deliberately separate from synchronized configuration.
// Each draft has an atomic manifest and immutable attachment snapshots, so typing
// never rewrites attachment bytes or relies on the original file staying around.
class ComposerDraftStore {
public:
    ComposerDraftStore();
    static QString directory();
    // Blocking fixture inspection/setup only; never call these on GUI paths.
    ComposerDraft load(const QString &key) const;
    bool save(const QString &key, ComposerDraft &draft) const;
    // Synchronous helpers above and flushWrites are for isolated tests/shutdown.
    // GUI callers use ordered asynchronous operations; durability never coalesces.
    void saveAsync(const QString &key, const ComposerDraft &draft, QObject *owner,
                   std::function<void(bool, const ComposerDraft &)> done) const;
    void saveDurableAsync(const QString &key, const ComposerDraft &draft, QObject *owner,
                          std::function<void(bool, const ComposerDraft &)> done) const;
    void loadAsync(const QString &key, QObject *owner, std::function<void(const ComposerDraft &)> done) const;
    void indexAsync(QObject *owner, std::function<void(const QHash<QString, ComposerDraftSummary> &)> done) const;
    void renameAsync(const QString &oldKey, const QString &newKey, const ComposerDraft &draft, QObject *owner,
                     std::function<void(bool, const ComposerDraft &)> done) const;
    static ComposerDraftOwner owner(const QString &key);
    static void flushWrites();
    static void setWriteObserver(std::function<void()> observer); // isolated tests
    // Blocking fixture metadata inspection only.
    QStringList keys() const;
    QString label(const QString &key) const;
private:
    ComposerDraft loadDirect(const QString &key) const;
    QHash<QString, ComposerDraftSummary> indexDirect() const;
    bool saveDirect(const QString &key, ComposerDraft &draft) const;
    QString root() const;
    QString folder(const QString &key) const;
    QString m_root;
};
