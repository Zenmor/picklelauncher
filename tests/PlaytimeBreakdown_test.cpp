// SPDX-License-Identifier: GPL-3.0-only

#include <QTest>
#include <limits>

#include "PlaytimeBreakdown.h"
#include "PlaytimeLedger.h"

using PlaytimeBreakdown::Entry;

namespace {

qint64 sumOfRows(const PlaytimeBreakdown::Result& result)
{
    qint64 sum = 0;
    for (const auto& row : result.rows)
        sum += row.seconds;
    return sum;
}

double sumOfPercent(const PlaytimeBreakdown::Result& result)
{
    double sum = 0;
    for (const auto& row : result.rows)
        sum += row.percent;
    return sum;
}

constexpr qint64 kIntMax = (std::numeric_limits<int>::max)();

}  // namespace

class PlaytimeBreakdownTest : public QObject {
    Q_OBJECT

   private slots:
    void noEntries_noRows()
    {
        const auto result = PlaytimeBreakdown::build({});
        QVERIFY(result.rows.isEmpty());
        QCOMPARE(result.totalSeconds, qint64(0));
    }

    void entriesWithoutTime_areDropped()
    {
        const auto result = PlaytimeBreakdown::build({ { "a", "A", 0 }, { "b", "B", -5 }, { "c", "C", 10 } });
        QCOMPARE(result.rows.size(), 1);
        QCOMPARE(result.rows[0].key, QString("c"));
        QCOMPARE(result.totalSeconds, qint64(10));
    }

    void sameKey_isAddedUp()
    {
        const auto result =
            PlaytimeBreakdown::build({ { "modrinth:x", "Pack", 100 }, { "modrinth:x", "Pack", 50 }, { "other", "Other", 20 } });
        QCOMPARE(result.rows.size(), 2);
        QCOMPARE(result.rows[0].key, QString("modrinth:x"));
        QCOMPARE(result.rows[0].seconds, qint64(150));
        QCOMPARE(result.totalSeconds, qint64(170));
    }

    void sameKey_keepsTheFirstNameThatIsNotEmpty()
    {
        const auto result = PlaytimeBreakdown::build({ { "k", "", 1 }, { "k", "Current name", 1 }, { "k", "Old name", 1 } });
        QCOMPARE(result.rows.size(), 1);
        QCOMPARE(result.rows[0].name, QString("Current name"));
    }

    void emptyName_showsTheKey()
    {
        const auto result = PlaytimeBreakdown::build({ { "modrinth:x", "", 5 } });
        QCOMPARE(result.rows[0].name, QString("modrinth:x"));
    }

    void rows_mostTimeFirst_tiesByName()
    {
        const auto result = PlaytimeBreakdown::build({ { "1", "beta", 10 }, { "2", "Zed", 30 }, { "3", "Alpha", 10 }, { "4", "mid", 20 } });
        QStringList names;
        for (const auto& row : result.rows)
            names << row.name;
        QCOMPARE(names, QStringList({ "Zed", "mid", "Alpha", "beta" }));
    }

    void percent_isTheShareOfTheRows()
    {
        const auto result = PlaytimeBreakdown::build({ { "a", "A", 750 }, { "b", "B", 250 } });
        QCOMPARE(result.rows[0].percent, 75.0);
        QCOMPARE(result.rows[1].percent, 25.0);
        QVERIFY(qAbs(sumOfPercent(result) - 100.0) < 1e-9);
    }

    // The real layout of the dialog: a pack (live instances + ledger), a custom instance, a hidden one, deleted custom ones.
    void rowsAddUpToTheTotal()
    {
        PlaytimeLedger ledger;
        ledger.add("modrinth:pack", 600, "Pack");
        ledger.add("flame:gone", 120, "Gone pack");
        ledger.add({}, 300);

        QList<Entry> entries;
        entries.append({ "modrinth:pack", "Pack", 3600 });  // live instance, pack version 1
        entries.append({ "modrinth:pack", "Pack", 1800 });  // live instance, pack version 2
        entries.append({ "instance:TestCustom", "TestCustom", 900 });
        entries.append({ "instance:Quickplay", "Quickplay", 42 });  // hidden instances count too
        for (const auto& pack : ledger.packEntries())
            entries.append({ pack.key, pack.name, pack.seconds });
        entries.append({ QString(PlaytimeBreakdown::DeletedCustomKey), "", ledger.anonymousSeconds() });

        const auto result = PlaytimeBreakdown::build(entries);

        QCOMPARE(result.rows.size(), 5);
        QCOMPARE(result.rows[0].key, QString("modrinth:pack"));
        QCOMPARE(result.rows[0].seconds, qint64(3600 + 1800 + 600));  // the "pack total played" number
        QCOMPARE(result.totalSeconds, qint64(3600 + 1800 + 900 + 42) + ledger.totalSeconds());
        QCOMPARE(sumOfRows(result), result.totalSeconds);
        QVERIFY(qAbs(sumOfPercent(result) - 100.0) < 1e-9);
    }

    void deletedCustom_hasItsOwnRow()
    {
        const auto result = PlaytimeBreakdown::build({ { QString(PlaytimeBreakdown::DeletedCustomKey), "", 77 }, { "a", "A", 5 } });
        QCOMPARE(result.rows[0].key, QString(PlaytimeBreakdown::DeletedCustomKey));
        QCOMPARE(result.rows[0].seconds, qint64(77));
    }

    // InstanceList::getTotalPlayTime() is an int and stops at int max
    void total_isClampedToIntMax()
    {
        const auto result = PlaytimeBreakdown::build({ { "a", "A", kIntMax - 5 }, { "b", "B", 100 } });
        QCOMPARE(result.totalSeconds, kIntMax);
        QCOMPARE(sumOfRows(result), kIntMax - 5 + 100);  // the rows keep their real time, only the total is clamped
        QVERIFY(qAbs(sumOfPercent(result) - 100.0) < 1e-9);
    }

    void total_exactlyIntMax_isNotChanged()
    {
        const auto result = PlaytimeBreakdown::build({ { "a", "A", kIntMax - 1 }, { "b", "B", 1 } });
        QCOMPARE(result.totalSeconds, kIntMax);
        QCOMPARE(sumOfRows(result), kIntMax);
    }

    void hugeEntries_doNotOverflow()
    {
        const qint64 huge = (std::numeric_limits<qint64>::max)();
        const auto result = PlaytimeBreakdown::build({ { "a", "A", huge }, { "a", "A", huge }, { "b", "B", huge } });
        QCOMPARE(result.totalSeconds, kIntMax);
        QCOMPARE(result.rows[0].seconds, huge);
        QVERIFY(result.rows[0].percent > 0);
    }

    void ledgerPackEntries_listsEveryPackOnceWithoutTheAnonymousAmount()
    {
        PlaytimeLedger ledger;
        QVERIFY(ledger.packEntries().isEmpty());

        ledger.add("modrinth:a", 10, "A");
        ledger.add("modrinth:a", 5, "A renamed");
        ledger.add("flame:b", 7);
        ledger.add({}, 100);

        const auto entries = ledger.packEntries();
        QCOMPARE(entries.size(), 2);
        qint64 sum = 0;
        for (const auto& entry : entries) {
            QCOMPARE(entry.seconds, ledger.packSeconds(entry.key));
            sum += entry.seconds;
        }
        QCOMPARE(sum, ledger.totalSeconds() - ledger.anonymousSeconds());

        ledger.subtract("flame:b", 7);  // a pack that reaches zero is gone from the entries
        QCOMPARE(ledger.packEntries().size(), 1);
    }
};

QTEST_GUILESS_MAIN(PlaytimeBreakdownTest)

#include "PlaytimeBreakdown_test.moc"
