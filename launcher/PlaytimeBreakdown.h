// SPDX-License-Identifier: GPL-3.0-only
/*
 *  picklelauncher - where the playtime went (pickle-playtime)
 *
 *  Pure aggregation, no GUI and no instance list: the caller says how much time belongs to what, this sorts it and
 *  works out the shares. The total follows the rule of InstanceList::getTotalPlayTime(), including its clamp.
 */

#pragma once

#include <QList>
#include <QString>

namespace PlaytimeBreakdown {

/** Key of the time of deleted instances that were not part of a modpack (the ledger cannot split it by name). */
inline constexpr QLatin1String DeletedCustomKey{ "deleted-custom" };

/** Time that belongs to one thing. Entries with the same key are added up. */
struct Entry {
    QString key;
    QString name;  // empty: the key is shown
    qint64 seconds = 0;
};

struct Row {
    QString key;
    QString name;
    qint64 seconds = 0;
    double percent = 0;  // of the sum of all rows, so the percentages add up to 100
};

struct Result {
    QList<Row> rows;          // most time first, then by name
    qint64 totalSeconds = 0;  // sum of the rows, at most int max: the number InstanceList::getTotalPlayTime() returns
};

/** Entries with no time (zero or negative) are dropped. */
Result build(const QList<Entry>& entries);

}  // namespace PlaytimeBreakdown
