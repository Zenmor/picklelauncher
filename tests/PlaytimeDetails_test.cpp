// SPDX-License-Identifier: GPL-3.0-only

#include <QTest>

#include "PlaytimeDetails.h"

using PlaytimeDetails::InstanceRecord;
using PlaytimeDetails::Moment;

namespace {

constexpr qint64 Day = 24LL * 3600 * 1000;

InstanceRecord played(const QString& name, qint64 first, qint64 last, qint64 birth = 0, qint64 seconds = 60)
{
    InstanceRecord record;
    record.name = name;
    record.firstLaunchMs = first;
    record.lastLaunchMs = last;
    record.folderBirthMs = birth;
    record.seconds = seconds;
    return record;
}

PlaytimeDetails::Input pack(const QList<InstanceRecord>& instances, qint64 deleted = 0)
{
    PlaytimeDetails::Input input;
    input.name = "Test Pack";
    input.packType = "modrinth";
    input.instances = instances;
    input.deletedSeconds = deleted;
    return input;
}

Moment known(qint64 ms, bool estimated = false)
{
    return { Moment::State::Known, ms, estimated };
}

}  // namespace

class PlaytimeDetailsTest : public QObject {
    Q_OBJECT

   private slots:
    void earliestAndLatest_overSeveralInstances()
    {
        const auto details = PlaytimeDetails::build(pack({
            played("b", 5 * Day, 9 * Day),
            played("a", 2 * Day, 7 * Day),
            played("c", 3 * Day, 12 * Day),
        }));
        QCOMPARE(details.firstPlayed, known(2 * Day));
        QCOMPARE(details.lastPlayed, known(12 * Day));
    }

    void zeroMeansUnknown_notTheEpoch()
    {
        // an instance with no dates must not drag the earliest down to 0 or the latest down
        const auto details = PlaytimeDetails::build(pack({
            played("a", 0, 0, 0, 60),
            played("b", 4 * Day, 6 * Day),
        }));
        QCOMPARE(details.firstPlayed, known(4 * Day));
        QCOMPARE(details.lastPlayed, known(6 * Day));
    }

    void recordedTime_beatsAnEstimateOfTheSameInstance()
    {
        QCOMPARE(PlaytimeDetails::firstPlayed(played("a", 5 * Day, 9 * Day, 1 * Day)), known(5 * Day));
    }

    void recordedTime_winsATieWithAnEstimate()
    {
        const auto details = PlaytimeDetails::build(pack({
            played("a", 0, 9 * Day, 3 * Day),  // estimate 3 Day
            played("b", 3 * Day, 8 * Day),     // recorded 3 Day
        }));
        QCOMPARE(details.firstPlayed, known(3 * Day, false));
    }

    void earlierEstimate_ofAnotherInstance_isShownAsEstimate()
    {
        const auto details = PlaytimeDetails::build(pack({
            played("a", 0, 9 * Day, 1 * Day),
            played("b", 3 * Day, 8 * Day),
        }));
        QCOMPARE(details.firstPlayed, known(1 * Day, true));
    }

    void estimate_usesTheFolderDate()
    {
        QCOMPARE(PlaytimeDetails::firstPlayed(played("a", 0, 9 * Day, 4 * Day)), known(4 * Day, true));
        QCOMPARE(PlaytimeDetails::firstPlayed(played("a", 0, 9 * Day, 9 * Day)), known(9 * Day, true));  // the same moment is fine
    }

    void estimate_isRejected_whenTheFolderIsYoungerThanTheLastLaunch()
    {
        const auto first = PlaytimeDetails::firstPlayed(played("a", 0, 5 * Day, 6 * Day));
        QCOMPARE(first.state, Moment::State::NotRecorded);
    }

    void estimate_isRejected_withoutLastLaunchOrFolderDate()
    {
        QCOMPARE(PlaytimeDetails::firstPlayed(played("a", 0, 0, 4 * Day, 60)).state, Moment::State::NotRecorded);
        QCOMPARE(PlaytimeDetails::firstPlayed(played("a", 0, 9 * Day, 0)).state, Moment::State::NotRecorded);
    }

    void neverPlayed()
    {
        const auto details = PlaytimeDetails::build(pack({ played("a", 0, 0, 2 * Day, 0), played("b", 0, 0, 3 * Day, 0) }));
        QCOMPARE(details.firstPlayed.state, Moment::State::NeverPlayed);
        QCOMPARE(details.lastPlayed.state, Moment::State::NeverPlayed);
    }

    void playedWithoutAnyDate_isNotRecorded()
    {
        const auto details = PlaytimeDetails::build(pack({ played("a", 0, 0, 0, 60), played("b", 0, 0, 0, 0) }));
        QCOMPARE(details.firstPlayed.state, Moment::State::NotRecorded);
        QCOMPARE(details.lastPlayed.state, Moment::State::NotRecorded);
    }

    void aKnownDate_beatsNotRecordedOfAnotherInstance()
    {
        const auto details = PlaytimeDetails::build(pack({ played("a", 0, 0, 0, 60), played("b", 4 * Day, 6 * Day) }));
        QCOMPARE(details.firstPlayed, known(4 * Day));
        QCOMPARE(details.lastPlayed, known(6 * Day));
    }

    void loaderUid_mapsToAName()
    {
        QCOMPARE(PlaytimeDetails::loaderName("net.minecraftforge"), QString("Forge"));
        QCOMPARE(PlaytimeDetails::loaderName("net.neoforged"), QString("NeoForge"));
        QCOMPARE(PlaytimeDetails::loaderName("net.fabricmc.fabric-loader"), QString("Fabric"));
        QCOMPARE(PlaytimeDetails::loaderName("org.quiltmc.quilt-loader"), QString("Quilt"));
        QCOMPARE(PlaytimeDetails::loaderName("com.mumfrey.liteloader"), QString("LiteLoader"));
        QVERIFY(PlaytimeDetails::loaderName("net.fabricmc.intermediary").isEmpty());
        QVERIFY(PlaytimeDetails::loaderName({}).isEmpty());
    }

    void sourceType_mapsToAName_andAnUnknownOnePassesThrough()
    {
        QCOMPARE(PlaytimeDetails::sourceName("modrinth"), QString("Modrinth"));
        QCOMPARE(PlaytimeDetails::sourceName("flame"), QString("CurseForge"));
        QCOMPARE(PlaytimeDetails::sourceName("ftb"), QString("FTB"));
        QCOMPARE(PlaytimeDetails::sourceName("atlauncher"), QString("ATLauncher"));
        QCOMPARE(PlaytimeDetails::sourceName("somewhere-new"), QString("somewhere-new"));
        QVERIFY(PlaytimeDetails::sourceName({}).isEmpty());

        auto input = pack({});
        input.packType = "somewhere-new";
        QCOMPARE(PlaytimeDetails::build(input).sourceName, QString("somewhere-new"));
    }

    void instances_areListedByNameWithTheirVersions()
    {
        auto v1 = played("Test Pack v2", 2 * Day, 3 * Day);
        v1.packVersion = "2.0";
        v1.minecraftVersion = "1.20.1";
        v1.loaderUid = "net.fabricmc.fabric-loader";
        v1.loaderVersion = "0.16.5";
        auto v2 = played("test pack v1", 1 * Day, 2 * Day);
        v2.packVersion = "1.0";
        v2.minecraftVersion = "1.19.2";

        const auto details = PlaytimeDetails::build(pack({ v1, v2 }));
        QCOMPARE(details.instances.size(), 2);
        QCOMPARE(details.instances[0].name, QString("test pack v1"));
        QCOMPARE(details.instances[0].packVersion, QString("1.0"));
        QVERIFY(details.instances[0].loaderName.isEmpty());
        QCOMPARE(details.instances[1].packVersion, QString("2.0"));
        QCOMPARE(details.instances[1].minecraftVersion, QString("1.20.1"));
        QCOMPARE(details.instances[1].loaderName, QString("Fabric"));
        QCOMPARE(details.instances[1].loaderVersion, QString("0.16.5"));
    }

    void customInstance_isNotAPack()
    {
        PlaytimeDetails::Input input;
        input.name = "Test Custom";
        input.instances = { played("Test Custom", 1 * Day, 2 * Day) };
        const auto details = PlaytimeDetails::build(input);
        QVERIFY(!details.isPack);
        QVERIFY(details.sourceName.isEmpty());
        QCOMPARE(details.instances.size(), 1);
    }

    void packOnlyInTheLedger()
    {
        const auto details = PlaytimeDetails::build(pack({}, 5400));
        QVERIFY(details.isPack);
        QCOMPARE(details.name, QString("Test Pack"));
        QCOMPARE(details.sourceName, QString("Modrinth"));
        QVERIFY(details.instances.isEmpty());
        QCOMPARE(details.deletedSeconds, qint64(5400));
        QCOMPARE(details.firstPlayed.state, Moment::State::NeverPlayed);
        QCOMPARE(details.lastPlayed.state, Moment::State::NeverPlayed);
    }

    void deletedSeconds_neverNegative() { QCOMPARE(PlaytimeDetails::build(pack({}, -3)).deletedSeconds, qint64(0)); }
};

QTEST_GUILESS_MAIN(PlaytimeDetailsTest)

#include "PlaytimeDetails_test.moc"
