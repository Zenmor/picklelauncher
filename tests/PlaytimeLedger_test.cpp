// SPDX-License-Identifier: GPL-3.0-only

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <limits>

#include "PlaytimeLedger.h"

namespace {

void writeFile(const QString& path, const QByteArray& data)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(data);
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

}  // namespace

class PlaytimeLedgerTest : public QObject {
    Q_OBJECT

   private slots:
    void packKey_needsTypeAndId()
    {
        QCOMPARE(PlaytimeLedger::packKey("modrinth", "AANobbMI"), QString("modrinth:AANobbMI"));
        QCOMPARE(PlaytimeLedger::packKey("flame", "396246"), QString("flame:396246"));
        QVERIFY(PlaytimeLedger::packKey("", "x").isEmpty());
        QVERIFY(PlaytimeLedger::packKey("modrinth", "").isEmpty());
        QVERIFY(PlaytimeLedger::packKey("", "").isEmpty());
    }

    void add_perPackAnonymousAndTotal()
    {
        PlaytimeLedger ledger;
        QVERIFY(ledger.isEmpty());

        ledger.add("modrinth:a", 100, "Pack A");
        ledger.add("modrinth:a", 50);  // another version of the same pack
        ledger.add("flame:7", 10);
        ledger.add({}, 5);  // no pack identity
        ledger.add({}, 1);

        QCOMPARE(ledger.packSeconds("modrinth:a"), 150);
        QCOMPARE(ledger.packSeconds("flame:7"), 10);
        QCOMPARE(ledger.packSeconds("flame:unknown"), 0);
        QCOMPARE(ledger.packSeconds({}), 0);  // the anonymous amount is not a pack
        QCOMPARE(ledger.anonymousSeconds(), 6);
        QCOMPARE(ledger.totalSeconds(), 166);
        QVERIFY(!ledger.isEmpty());
    }

    void add_ignoresZeroAndNegative()
    {
        PlaytimeLedger ledger;
        ledger.add("modrinth:a", 0);
        ledger.add("modrinth:a", -30);
        ledger.add({}, -1);
        QVERIFY(ledger.isEmpty());
        QCOMPARE(ledger.totalSeconds(), 0);
    }

    void subtract_takesWhatWasAddedBackOut()
    {
        PlaytimeLedger ledger;
        ledger.add("modrinth:a", 100, "Pack A");
        ledger.add("modrinth:a", 50);
        ledger.add({}, 5);

        const auto before = ledger;
        ledger.add("modrinth:a", 30);
        ledger.add({}, 7);
        ledger.subtract("modrinth:a", 30);
        ledger.subtract({}, 7);
        QVERIFY(ledger == before);
        QCOMPARE(ledger.packSeconds("modrinth:a"), 150);
    }

    void subtract_neverGoesBelowZeroAndDropsEmptyPacks()
    {
        PlaytimeLedger ledger;
        ledger.add("modrinth:a", 10, "Pack A");
        ledger.add({}, 4);

        ledger.subtract("modrinth:a", 25);  // more than there is
        ledger.subtract({}, 25);
        QCOMPARE(ledger.packSeconds("modrinth:a"), 0);
        QCOMPARE(ledger.anonymousSeconds(), 0);
        QVERIFY(ledger.isEmpty());  // the pack is gone, not kept with zero seconds
        QVERIFY(ledger.toJson()["packs"].toObject().isEmpty());

        // nothing to take out of an unknown pack, zero and negative amounts do nothing
        ledger.add("flame:7", 5);
        ledger.subtract("flame:unknown", 5);
        ledger.subtract("flame:7", 0);
        ledger.subtract("flame:7", -3);
        QCOMPARE(ledger.packSeconds("flame:7"), 5);
    }

    void add_doesNotOverflow()
    {
        PlaytimeLedger ledger;
        ledger.add("modrinth:a", std::numeric_limits<qint64>::max() - 1);
        ledger.add("modrinth:a", 100);
        QCOMPARE(ledger.packSeconds("modrinth:a"), std::numeric_limits<qint64>::max());
        ledger.add({}, 100);
        QCOMPARE(ledger.totalSeconds(), std::numeric_limits<qint64>::max());
    }

    void json_roundTrip()
    {
        PlaytimeLedger ledger;
        ledger.add("modrinth:a", 100, "Pack A");
        ledger.add("flame:7", 10);
        ledger.add({}, 5);

        PlaytimeLedger loaded;
        QVERIFY(loaded.fromJson(ledger.toJson()));
        QVERIFY(loaded == ledger);
        QCOMPARE(loaded.packSeconds("modrinth:a"), 100);
        QCOMPARE(loaded.anonymousSeconds(), 5);
    }

    void fromJson_rejectsWhatIsNotALedger()
    {
        const QList<QByteArray> bad{
            R"({})",
            R"({"version":2,"anonymous":0,"packs":{}})",                      // a newer format
            R"({"version":1,"packs":{}})",                                    // no anonymous amount
            R"({"version":1,"anonymous":-1,"packs":{}})",                     // negative
            R"({"version":1,"anonymous":1.5,"packs":{}})",                    // not whole seconds
            R"({"version":1,"anonymous":"3","packs":{}})",                    // text
            R"({"version":1,"anonymous":0,"packs":[]})",                      // wrong container
            R"({"version":1,"anonymous":0,"packs":{"a:b":5}})",               // pack is not an object
            R"({"version":1,"anonymous":0,"packs":{"a:b":{"seconds":-5}}})",  // negative pack time
            R"({"version":1,"anonymous":0,"packs":{"a:b":{"name":"x"}}})",    // no seconds
            R"({"version":1,"anonymous":0,"packs":{"":{"seconds":5}}})",      // empty key
        };
        for (const auto& json : bad) {
            PlaytimeLedger ledger;
            ledger.add("modrinth:keep", 7);
            QVERIFY2(!ledger.fromJson(QJsonDocument::fromJson(json).object()), json.constData());
            // a failed read does not damage what the ledger had
            QCOMPARE(ledger.packSeconds("modrinth:keep"), 7);
        }
    }

    void saveAndLoad_roundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath("sub/playtime-ledger.json");  // the folder is created

        PlaytimeLedger ledger;
        ledger.add("modrinth:a", 100, "Pack A");
        ledger.add({}, 5);
        QVERIFY(ledger.save(path));

        PlaytimeLedger loaded;
        QCOMPARE(loaded.load(path), PlaytimeLedger::LoadResult::Loaded);
        QVERIFY(loaded == ledger);
    }

    void load_missingFileIsAnEmptyLedger()
    {
        QTemporaryDir dir;
        PlaytimeLedger ledger;
        ledger.add("modrinth:a", 1);
        QCOMPARE(ledger.load(dir.filePath("nothing.json")), PlaytimeLedger::LoadResult::Missing);
        QVERIFY(ledger.isEmpty());
    }

    void save_keepsThePreviousFileAsBackup()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("ledger.json");

        PlaytimeLedger first;
        first.add("modrinth:a", 100);
        QVERIFY(first.save(path));
        QVERIFY(!QFile::exists(path + ".bak"));  // nothing to keep yet

        PlaytimeLedger second = first;
        second.add("modrinth:a", 50);
        QVERIFY(second.save(path));

        PlaytimeLedger backup;
        QCOMPARE(backup.load(path + ".bak"), PlaytimeLedger::LoadResult::Loaded);
        QCOMPARE(backup.packSeconds("modrinth:a"), 100);

        PlaytimeLedger current;
        QCOMPARE(current.load(path), PlaytimeLedger::LoadResult::Loaded);
        QCOMPARE(current.packSeconds("modrinth:a"), 150);
    }

    void load_fallsBackToTheBackupWhenTheFileIsDamaged()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("ledger.json");

        PlaytimeLedger first;
        first.add("modrinth:a", 100);
        QVERIFY(first.save(path));
        PlaytimeLedger second = first;
        second.add("modrinth:a", 50);
        QVERIFY(second.save(path));

        writeFile(path, "{ this is not json");  // a crash while writing, a disk error, an editor

        PlaytimeLedger loaded;
        QCOMPARE(loaded.load(path), PlaytimeLedger::LoadResult::FromBackup);
        QCOMPARE(loaded.packSeconds("modrinth:a"), 100);
    }

    void load_unusableFilesAreLeftAlone()
    {
        QTemporaryDir dir;
        const QString path = dir.filePath("ledger.json");
        const QByteArray future = R"({"version":99,"anonymous":0,"packs":{}})";
        writeFile(path, future);

        PlaytimeLedger ledger;
        ledger.add("modrinth:a", 1);
        QCOMPARE(ledger.load(path), PlaytimeLedger::LoadResult::Unusable);
        QVERIFY(ledger.isEmpty());

        // load never writes; whoever gets Unusable must not save over the file, and it is still as it was
        QCOMPARE(readFile(path), future);
        QVERIFY(!QFile::exists(path + ".bak"));
    }

    void save_failsWhenThePathCannotBeWritten()
    {
        QTemporaryDir dir;
        // a file where the folder should be
        writeFile(dir.filePath("blocker"), "x");

        PlaytimeLedger ledger;
        ledger.add("modrinth:a", 1);
        QVERIFY(!ledger.save(dir.filePath("blocker/ledger.json")));
    }
};

QTEST_GUILESS_MAIN(PlaytimeLedgerTest)

#include "PlaytimeLedger_test.moc"
