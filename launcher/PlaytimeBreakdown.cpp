// SPDX-License-Identifier: GPL-3.0-only

#include "PlaytimeBreakdown.h"

#include <QHash>
#include <algorithm>
#include <limits>

namespace {

qint64 saturatingAdd(qint64 a, qint64 b)
{
    return a > (std::numeric_limits<qint64>::max)() - b ? (std::numeric_limits<qint64>::max)() : a + b;
}

}  // namespace

PlaytimeBreakdown::Result PlaytimeBreakdown::build(const QList<Entry>& entries)
{
    Result result;
    QHash<QString, int> rowOfKey;
    qint64 sum = 0;

    for (const auto& entry : entries) {
        if (entry.seconds <= 0)
            continue;
        sum = saturatingAdd(sum, entry.seconds);

        const auto found = rowOfKey.constFind(entry.key);
        if (found == rowOfKey.constEnd()) {
            rowOfKey.insert(entry.key, static_cast<int>(result.rows.size()));
            result.rows.append(Row{ entry.key, entry.name, entry.seconds, 0.0 });
            continue;
        }
        auto& row = result.rows[*found];
        row.seconds = saturatingAdd(row.seconds, entry.seconds);
        if (row.name.isEmpty())
            row.name = entry.name;
    }

    for (auto& row : result.rows) {
        if (row.name.isEmpty())
            row.name = row.key;
        row.percent = static_cast<double>(row.seconds) * 100.0 / static_cast<double>(sum);
    }

    std::sort(result.rows.begin(), result.rows.end(), [](const Row& a, const Row& b) {
        if (a.seconds != b.seconds)
            return a.seconds > b.seconds;
        if (const int byName = a.name.compare(b.name, Qt::CaseInsensitive))
            return byName < 0;
        return a.key < b.key;
    });

    result.totalSeconds = std::min<qint64>(sum, (std::numeric_limits<int>::max)());
    return result;
}
