// SPDX-License-Identifier: GPL-3.0-only

#include <QTest>
#include <memory>

#include "meta/JsonFormat.h"
#include "meta/Version.h"
#include "meta/VersionList.h"
#include "quickplay/QuickplayLoader.h"

using namespace Quickplay;

namespace {

// A loader version as the metadata lists it. `minecraft` is the version it requires, empty for loaders that do not require one.
Meta::Version::Ptr makeVersion(const QString& uid,
                               const QString& version,
                               const QString& type,
                               qint64 time,
                               const QString& minecraft = QString(),
                               bool recommended = false)
{
    auto result = std::make_shared<Meta::Version>(uid, version);
    result->setType(type);
    result->setTime(time);
    Meta::RequireSet reqs;
    if (!minecraft.isEmpty())
        reqs.insert(Meta::Require{ "net.minecraft", minecraft, QString() });
    result->setRequires(reqs, {});
    result->setRecommended(recommended);
    return result;
}

Meta::VersionList::Ptr makeList(const QString& uid, const QList<Meta::Version::Ptr>& versions)
{
    auto list = std::make_shared<Meta::VersionList>(uid);
    list->setVersions(versions);
    return list;
}

QString pick(Loader loader, const QString& minecraft, const QList<Meta::VersionList::Ptr>& lists)
{
    const auto version = pickLoaderVersion(loader, minecraft, lists);
    return version ? version->descriptor() : QString();
}

}  // namespace

class QuickplayLoaderTest : public QObject {
    Q_OBJECT

   private slots:
    void fabricAndQuilt_supportWhatTheIntermediaryMaps()
    {
        const auto intermediary =
            makeList("net.fabricmc.intermediary", { makeVersion("net.fabricmc.intermediary", "1.21", "release", 1, "1.21"),
                                                    makeVersion("net.fabricmc.intermediary", "1.21.1", "release", 2, "1.21.1") });
        const auto fabric = makeList("net.fabricmc.fabric-loader", { makeVersion("net.fabricmc.fabric-loader", "0.16.0", "release", 1) });
        const auto quilt = makeList("org.quiltmc.quilt-loader", { makeVersion("org.quiltmc.quilt-loader", "0.27.0", "release", 1) });

        const QSet<QString> expected{ "1.21", "1.21.1" };
        QCOMPARE(supportedMinecraftVersions(Loader::Fabric, { fabric, intermediary }), expected);
        QCOMPARE(supportedMinecraftVersions(Loader::Quilt, { quilt, intermediary }), expected);
    }

    void fabricAndQuilt_pickTheNewestRelease()
    {
        const auto intermediary =
            makeList("net.fabricmc.intermediary", { makeVersion("net.fabricmc.intermediary", "1.21", "release", 1, "1.21") });
        const auto quilt = makeList("org.quiltmc.quilt-loader", { makeVersion("org.quiltmc.quilt-loader", "0.30.0", "release", 100),
                                                                  makeVersion("org.quiltmc.quilt-loader", "0.29.0", "release", 50),
                                                                  makeVersion("org.quiltmc.quilt-loader", "0.31.0-beta.1", "beta", 200) });
        QCOMPARE(pick(Loader::Quilt, "1.21", { quilt, intermediary }), QString("0.30.0"));  // a newer beta does not win

        const auto onlyBeta =
            makeList("org.quiltmc.quilt-loader", { makeVersion("org.quiltmc.quilt-loader", "0.31.0-beta.1", "beta", 200),
                                                   makeVersion("org.quiltmc.quilt-loader", "0.31.0-beta.2", "beta", 300) });
        QCOMPARE(pick(Loader::Quilt, "1.21", { onlyBeta, intermediary }), QString("0.31.0-beta.2"));
    }

    void forge_supportsWhatItsVersionsRequire()
    {
        const auto forge = makeList("net.minecraftforge", { makeVersion("net.minecraftforge", "51.0.0", "", 1, "1.21"),
                                                            makeVersion("net.minecraftforge", "52.0.1", "", 2, "1.21.1"),
                                                            makeVersion("net.minecraftforge", "52.1.0", "", 3, "1.21.1", true),
                                                            makeVersion("net.minecraftforge", "52.1.5", "", 4, "1.21.1") });
        QCOMPARE(supportedMinecraftVersions(Loader::Forge, { forge }), (QSet<QString>{ "1.21", "1.21.1" }));

        QCOMPARE(pick(Loader::Forge, "1.21.1", { forge }), QString("52.1.0"));  // the recommended one, not the newest
        QCOMPARE(pick(Loader::Forge, "1.21", { forge }), QString("51.0.0"));    // nothing recommended: the newest for that version
        QCOMPARE(pick(Loader::Forge, "1.20", { forge }), QString());            // no version for it
    }

    void neoForge_prefersAReleaseOverANewerBeta()
    {
        const auto neo = makeList("net.neoforged", { makeVersion("net.neoforged", "21.0.1", "release", 1, "1.21"),
                                                     makeVersion("net.neoforged", "21.0.2-beta", "beta", 2, "1.21"),
                                                     makeVersion("net.neoforged", "26.3.0.1-beta", "beta", 3, "26.3"),
                                                     makeVersion("net.neoforged", "26.3.0.2-beta", "beta", 4, "26.3") });
        QCOMPARE(pick(Loader::NeoForge, "1.21", { neo }), QString("21.0.1"));
        QCOMPARE(pick(Loader::NeoForge, "26.3", { neo }), QString("26.3.0.2-beta"));  // only betas exist for it
    }

    void none_andMissingLists()
    {
        QVERIFY(supportedMinecraftVersions(Loader::None, {}).isEmpty());
        QVERIFY(pickLoaderVersion(Loader::None, "1.21", {}) == nullptr);

        const auto fabric = makeList("net.fabricmc.fabric-loader", { makeVersion("net.fabricmc.fabric-loader", "0.16.0", "release", 1) });
        QVERIFY(supportedMinecraftVersions(Loader::Fabric, { fabric, nullptr }).isEmpty());  // the intermediary list is unknown
        QVERIFY(pickLoaderVersion(Loader::Forge, "1.21", { nullptr }) == nullptr);
    }
};

QTEST_GUILESS_MAIN(QuickplayLoaderTest)

#include "QuickplayLoader_test.moc"
