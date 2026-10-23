#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QLocale>
#include <QObject>

namespace WorktreeReviewUi {
inline bool hasReason(const QJsonObject &row, const QStringList &codes) {
    for (const auto &value : row["reasons"].toArray()) if (codes.contains(value.toObject()["code"].toString())) return true;
    return false;
}
inline bool localData(const QJsonObject &row) {
    return row["ignored_count"].toInt() > 0 || !row["ignored"].toArray().isEmpty() || row["ignored_bytes"].toDouble() > 0;
}
inline int category(const QJsonObject &row, bool verified = true) {
    if (row["kind"] != "linked") return 4;
    if (localData(row) && row["ignored_fingerprint"].toString().isEmpty()) return 3;
    if (!verified || row["verdict"].toString().isEmpty() || hasReason(row, {"status_unavailable", "processes_unavailable", "integration_unavailable", "ignored_unavailable"})) return 3;
    if (hasReason(row, {"processes", "sessions", "locked", "nested", "submodules"})) return 2;
    if ((row["verdict"] == "ready" && !localData(row)) || row["verdict"] == "missing") return 0;
    return 1;
}
inline QString categoryName(int value) {
    switch (value) {
    case 0: return QObject::tr("Cleanup candidates");
    case 1: return QObject::tr("Review work");
    case 2: return QObject::tr("In use");
    case 3: return QObject::tr("Not checked");
    default: return QObject::tr("Main folders");
    }
}
inline QString integration(const QJsonObject &row) {
    const auto data = row["integration"].toObject(); const auto base = data["base"].toString(); const auto kind = data["kind"].toString();
    if (kind == "ancestor") return QObject::tr("Merged into %1").arg(base);
    if (kind == "same_tree") return QObject::tr("Same files as %1; original branch kept").arg(base);
    if (kind == "equivalent_patches") return QObject::tr("Equivalent patches in %1 history; original branch kept").arg(base);
    if (kind == "remaining") return QObject::tr("Changes to compare with %1").arg(base);
    return row["merged"] == true ? QObject::tr("Merged") : QObject::tr("Integration not verified");
}
inline QString size(const QJsonObject &row) {
    if (row["verdict"] == "missing") return QObject::tr("Metadata only");
    if (!row.contains("disk_bytes")) return QObject::tr("Not measured");
    return (row["disk_complete"] == true ? QString() : QString::fromUtf8("≥ ")) + QLocale().formattedDataSize(qint64(row["disk_bytes"].toDouble()));
}
inline QString summary(const QJsonObject &row) {
    if (row["verdict"] == "missing") return QObject::tr("Folder is gone; forget its Git record");
    QStringList parts{integration(row)};
    if (localData(row) && row["ignored_fingerprint"].toString().isEmpty()) parts << QObject::tr("Update Zerus on this computer to review local files before cleanup");
    if (row["changes"].toInt()) parts << QObject::tr("%1 changed").arg(row["changes"].toInt());
    if (row["untracked"].toInt()) parts << QObject::tr("%1 untracked").arg(row["untracked"].toInt());
    if (localData(row)) parts << QObject::tr("Local files outside Git");
    for (const auto &value : row["reasons"].toArray()) {
        const auto reason = value.toObject();
        if (reason["code"] != "changes" && reason["code"] != "untracked") parts << reason["message"].toString();
    }
    return parts.join(QString::fromUtf8(" · "));
}
inline QStringList decision(const QJsonObject &row) {
    QStringList lines{QObject::tr("Code: %1 (local Git refs).").arg(integration(row))};
    if (row["verdict"] == "missing") {
        lines << QObject::tr("The folder is already gone. Cleanup only forgets its Git worktree record.");
        return lines;
    }
    if (row.contains("changes") && row.contains("untracked"))
        lines << (row["changes"].toInt() || row["untracked"].toInt()
            ? QObject::tr("Unsaved work: %1 changed and %2 untracked files.").arg(row["changes"].toInt()).arg(row["untracked"].toInt())
            : QObject::tr("Unsaved work: no changed or untracked files."));
    else lines << QObject::tr("Unsaved work: see the current file check below.");
    if (localData(row)) {
        lines << QObject::tr("Local data: %1 outside Git. These files may include caches, builds or unique data; Git cannot restore them.")
            .arg(QLocale().formattedDataSize(qint64(row["ignored_bytes"].toDouble())));
        lines << QObject::tr("Automatic cleanup keeps folders with any local data. Inspect the files before choosing to delete them.");
    } else if (row.contains("ignored_count")) lines << QObject::tr("Local data: no ignored files found.");
    for (const auto &value : row["reasons"].toArray()) lines << value.toObject()["message"].toString();
    for (const auto &value : row["notes"].toArray()) {
        const auto note = value.toObject();
        if (note["code"] != "ignored_data") lines << note["message"].toString();
    }
    lines.removeDuplicates();
    return lines;
}
inline bool removable(const QJsonObject &row) {
    return row["kind"] == "linked" && (row["verdict"] == "ready" || row["verdict"] == "review" || row["verdict"] == "missing")
        && !row["fingerprint"].toString().isEmpty() && (!localData(row) || !row["ignored_fingerprint"].toString().isEmpty());
}
}
