#pragma once
#include <QString>
#include <algorithm>

// The shape every layer accepts for an OpenCode effort (variant) ID, pinned by
// tests/fixtures/opencode-effort-ids.json: non-empty, at most 40 UTF-8 bytes,
// no whitespace, no control character and no '/'. Membership in the selected
// model's effort_options stays authoritative.
namespace OpenCodeEffort {
inline bool valid(const QString &id)
{
    return !id.isEmpty() && id.toUtf8().size() <= 40 && !id.contains(QLatin1Char('/'))
        && std::none_of(id.cbegin(), id.cend(), [](QChar c) { return c.isSpace() || c.category() == QChar::Other_Control; });
}
}
