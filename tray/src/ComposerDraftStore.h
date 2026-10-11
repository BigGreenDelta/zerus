#pragma once

#include "HgsClient.h"
#include <functional>

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

// Private desktop state, deliberately separate from synchronized configuration.
// Each draft has an atomic manifest and immutable attachment snapshots, so typing
// never rewrites attachment bytes or relies on the original file staying around.
class ComposerDraftStore {
public:
    static QString directory();
    ComposerDraft load(const QString &key) const;
    bool save(const QString &key, ComposerDraft &draft) const;
    // Ordinary edits enqueue/coalesce snapshots. Explicit send/discard/rename
    // uses save(), which orders a durable barrier after earlier writes.
    void saveAsync(const QString &key, const ComposerDraft &draft, QObject *owner,
                   std::function<void(bool, const ComposerDraft &)> done) const;
    static void flushWrites();
    static void setWriteObserver(std::function<void()> observer); // isolated tests
    QStringList keys() const;
    QString label(const QString &key) const;
private:
    bool saveDirect(const QString &key, ComposerDraft &draft) const;
    QString root() const;
    QString folder(const QString &key) const;
    QString m_root;
};
