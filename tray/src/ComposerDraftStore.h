#pragma once

#include "HgsClient.h"
#include <functional>
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
    void indexAsync(QObject *owner, std::function<void(const QHash<QString, QString> &)> done) const;
    void renameAsync(const QString &oldKey, const QString &newKey, const ComposerDraft &draft, QObject *owner,
                     std::function<void(bool, const ComposerDraft &)> done) const;
    static void flushWrites();
    static void setWriteObserver(std::function<void()> observer); // isolated tests
    // Blocking fixture metadata inspection only.
    QStringList keys() const;
    QString label(const QString &key) const;
private:
    ComposerDraft loadDirect(const QString &key) const;
    QHash<QString, QString> indexDirect() const;
    bool saveDirect(const QString &key, ComposerDraft &draft) const;
    QString root() const;
    QString folder(const QString &key) const;
    QString m_root;
};
