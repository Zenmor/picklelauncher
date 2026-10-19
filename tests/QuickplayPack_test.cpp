// SPDX-License-Identifier: GPL-3.0-only

#include <QTest>

#include "quickplay/QuickplayPack.h"

using ModPlatform::IndexedVersion;
using ModPlatform::IndexedVersionType;
using ModPlatform::ResourceProvider;
using namespace Quickplay;

namespace {

IndexedVersion version(const QString& id,
                       const QString& date,
                       IndexedVersionType type,
                       const QStringList& minecraft,
                       bool downloadable = true)
{
    IndexedVersion v;
    v.fileId = id;
    v.date = date;
    v.version_type = type;
    v.mcVersion = minecraft;
    v.downloadUrl = downloadable ? QStringLiteral("url-") + id : QString();
    return v;
}

QString pickedId(const QList<IndexedVersion>& versions, const QString& minecraft)
{
    const auto picked = pickPackVersion(versions, minecraft);
    return picked ? picked->fileId.toString() : QString();
}

}  // namespace

class QuickplayPackTest : public QObject {
    Q_OBJECT

   private slots:
    void pick_newestReleaseWins()
    {
        const QList<IndexedVersion> versions{
            version("old", "2026-01-01", IndexedVersionType::Release, { "1.21.1" }),
            version("new", "2026-03-01", IndexedVersionType::Release, { "1.21.1" }),
            version("other", "2026-04-01", IndexedVersionType::Release, { "1.20.1" }),
        };
        QCOMPARE(pickedId(versions, "1.21.1"), QString("new"));
        QCOMPARE(pickedId(versions, "1.20.1"), QString("other"));
    }

    void pick_aReleaseBeatsANewerBeta()
    {
        const QList<IndexedVersion> versions{
            version("beta", "2026-05-01", IndexedVersionType::Beta, { "1.21.1" }),
            version("release", "2026-01-01", IndexedVersionType::Release, { "1.21.1" }),
            version("alpha", "2026-06-01", IndexedVersionType::Alpha, { "1.21.1" }),
        };
        QCOMPARE(pickedId(versions, "1.21.1"), QString("release"));
    }

    void pick_withoutReleaseTheNewestOfAnyKind()
    {
        const QList<IndexedVersion> versions{
            version("alpha", "2026-06-01", IndexedVersionType::Alpha, { "26.3" }),
            version("beta", "2026-05-01", IndexedVersionType::Beta, { "26.3" }),
        };
        QCOMPARE(pickedId(versions, "26.3"), QString("alpha"));
    }

    void pick_skipsVersionsThatCannotBeDownloaded()
    {
        const QList<IndexedVersion> versions{
            version("blocked", "2026-06-01", IndexedVersionType::Release, { "1.21.1" }, false),
            version("open", "2026-01-01", IndexedVersionType::Release, { "1.21.1" }),
        };
        QCOMPARE(pickedId(versions, "1.21.1"), QString("open"));
    }

    void pick_nothingForAnUnsupportedVersion()
    {
        const QList<IndexedVersion> versions{ version("a", "2026-01-01", IndexedVersionType::Release, { "1.21.1" }) };
        QVERIFY(!pickPackVersion(versions, "1.8.9"));
        QVERIFY(!pickPackVersion({}, "1.21.1"));
    }

    void pick_aVersionForSeveralMinecraftVersions()
    {
        const QList<IndexedVersion> versions{ version("a", "2026-01-01", IndexedVersionType::Release, { "1.21", "1.21.1" }) };
        QCOMPARE(pickedId(versions, "1.21"), QString("a"));
        QCOMPARE(pickedId(versions, "1.21.1"), QString("a"));
    }

    void parse_modrinthList()
    {
        const auto versions = parsePackVersions(ResourceProvider::MODRINTH, R"([
            {"project_id": "P", "id": "v1", "date_published": "2026-03-01T00:00:00Z", "game_versions": ["1.21.1"], "loaders": ["fabric"],
             "name": "Pack 1", "version_number": "1.0", "version_type": "release", "dependencies": [],
             "files": [{"url": "u1", "filename": "pack.mrpack", "primary": true, "hashes": {"sha1": "a", "sha512": "b"}}]}
        ])");
        QVERIFY(versions);
        QCOMPARE(versions->size(), 1);
        QCOMPARE(versions->first().fileId.toString(), QString("v1"));
        QCOMPARE(versions->first().downloadUrl, QString("u1"));
        QVERIFY(versions->first().mcVersion.contains("1.21.1"));
        QVERIFY(versions->first().version_type == IndexedVersionType::Release);
    }

    void parse_curseForgeLeavesOutFilesWithoutUrl()
    {
        const auto versions = parsePackVersions(ResourceProvider::FLAME, R"({"data": [
            {"modId": 1, "id": 10, "fileDate": "2026-03-01T00:00:00Z", "displayName": "Pack 10", "fileName": "pack.zip", "releaseType": 1,
             "gameVersions": ["Fabric", "1.21.1"], "downloadUrl": "u10", "hashes": [], "dependencies": []},
            {"modId": 1, "id": 11, "fileDate": "2026-03-02T00:00:00Z", "displayName": "Pack 11", "fileName": "pack2.zip", "releaseType": 1,
             "gameVersions": ["Fabric", "1.21.1"], "downloadUrl": null, "hashes": [], "dependencies": []}
        ]})");
        QVERIFY(versions);
        QCOMPARE(versions->size(), 1);
        QCOMPARE(versions->first().fileId.toInt(), 10);
        QCOMPARE(versions->first().mcVersion, QStringList({ "1.21.1" }));
    }

    void parse_rejectsWhatIsNotAVersionList()
    {
        QVERIFY(!parsePackVersions(ResourceProvider::MODRINTH, "not json"));
        QVERIFY(!parsePackVersions(ResourceProvider::FLAME, R"({"error": "x"})"));
        const auto empty = parsePackVersions(ResourceProvider::MODRINTH, "[]");
        QVERIFY(empty && empty->isEmpty());  // an empty list is an answer: no versions
    }

    void minecraftVersions_modrinth()
    {
        const auto versions = packMinecraftVersions(ResourceProvider::MODRINTH, R"({"game_versions": ["1.21.1", "26.3"]})");
        QVERIFY(versions.contains("1.21.1"));
        QVERIFY(versions.contains("26.3"));
        QVERIFY(!versions.contains("1.8.9"));
    }

    void minecraftVersions_curseForgeSkipsLoaderNames()
    {
        const auto versions = packMinecraftVersions(
            ResourceProvider::FLAME,
            R"({"data": {"latestFilesIndexes": [{"gameVersion": "1.21.1"}, {"gameVersion": "Fabric"}, {"gameVersion": "26.3"}]}})");
        QCOMPARE(versions, QSet<QString>({ "1.21.1", "26.3" }));
    }

    void minecraftVersions_unknownAnswerGivesNothing()
    {
        QVERIFY(packMinecraftVersions(ResourceProvider::MODRINTH, "not json").isEmpty());
        QVERIFY(packMinecraftVersions(ResourceProvider::FLAME, "[]").isEmpty());
    }
};

QTEST_GUILESS_MAIN(QuickplayPackTest)

#include "QuickplayPack_test.moc"
