// SPDX-License-Identifier: GPL-3.0-only

#include <QTest>

#include "quickplay/QuickplayResolver.h"

using ModPlatform::Dependency;
using ModPlatform::DependencyType;
using ModPlatform::IndexedVersion;
using namespace Quickplay;

namespace {

Dependency dep(const QString& project, DependencyType type, const QString& versionId = {})
{
    Dependency dependency;
    dependency.addonId = project;
    dependency.type = type;
    dependency.version = versionId;
    return dependency;
}

IndexedVersion version(const QString& project, const QString& id, const QList<Dependency>& dependencies = {})
{
    IndexedVersion result;
    result.addonId = project;
    result.fileId = id;
    result.version = id;
    result.dependencies = dependencies;
    return result;
}

QHash<QString, QString> picked(const ResolveResult& result)
{
    QHash<QString, QString> out;
    for (const auto& mod : result.mods)
        out.insert(mod.projectId, mod.versionId);
    return out;
}

}  // namespace

class QuickplayResolverTest : public QObject {
    Q_OBJECT

   private slots:
    void picksNewestVersion()
    {
        VersionStore store{ { "A", { version("A", "a2"), version("A", "a1") } } };

        auto result = resolve({ "A" }, store);

        QCOMPARE(result.status, ResolveResult::Status::Done);
        QCOMPARE(picked(result), (QHash<QString, QString>{ { "A", "a2" } }));
        QVERIFY(!result.mods.first().isDependency);
    }

    void pullsInRequiredDependency()
    {
        VersionStore store{ { "A", { version("A", "a1", { dep("D", DependencyType::REQUIRED) }) } }, { "D", { version("D", "d1") } } };

        auto result = resolve({ "A" }, store);

        QCOMPARE(result.status, ResolveResult::Status::Done);
        QCOMPARE(picked(result), (QHash<QString, QString>{ { "A", "a1" }, { "D", "d1" } }));
        for (const auto& mod : result.mods)
            QCOMPARE(mod.isDependency, mod.projectId == "D");
    }

    void earlierProjectWinsIncompatibility()
    {
        VersionStore store{ { "A", { version("A", "a1", { dep("B", DependencyType::INCOMPATIBLE) }) } }, { "B", { version("B", "b1") } } };

        auto result = resolve({ "A", "B" }, store);

        QCOMPARE(result.status, ResolveResult::Status::Done);
        QCOMPARE(picked(result), (QHash<QString, QString>{ { "A", "a1" } }));
        QCOMPARE(result.skippedProjects, QStringList{ "B" });
    }

    void backtracksWhenRequiredDependencyHasNoVersion()
    {
        VersionStore store{ { "A", { version("A", "a2", { dep("C", DependencyType::REQUIRED) }), version("A", "a1") } }, { "C", {} } };

        auto result = resolve({ "A" }, store);

        QCOMPARE(result.status, ResolveResult::Status::Done);
        QCOMPARE(picked(result), (QHash<QString, QString>{ { "A", "a1" } }));
    }

    void prefersVersionOtherProjectsDependOn()
    {
        // X and Y only work with s1, so taking the newest S would drop both
        VersionStore store{
            { "S", { version("S", "s2"), version("S", "s1") } },
            { "X", { version("X", "x1", { dep("S", DependencyType::REQUIRED, "s1") }) } },
            { "Y", { version("Y", "y1", { dep("S", DependencyType::REQUIRED, "s1") }) } },
        };

        auto result = resolve({ "S", "X", "Y" }, store);

        QCOMPARE(result.status, ResolveResult::Status::Done);
        QCOMPARE(picked(result), (QHash<QString, QString>{ { "S", "s1" }, { "X", "x1" }, { "Y", "y1" } }));
        QVERIFY(result.skippedProjects.isEmpty());
    }

    void triesRecommendedVersionOfOptionalDependencyFirst()
    {
        VersionStore store{
            { "A", { version("A", "a1", { dep("B", DependencyType::OPTIONAL, "b1") }) } },
            { "B", { version("B", "b2"), version("B", "b1") } },
        };

        auto result = resolve({ "A", "B" }, store);

        QCOMPARE(result.status, ResolveResult::Status::Done);
        QCOMPARE(picked(result).value("B"), QString("b1"));
    }

    void asksForVersionsOfUnknownDependencies()
    {
        VersionStore store{ { "A", { version("A", "a1", { dep("C", DependencyType::REQUIRED) }) } } };

        auto result = resolve({ "A" }, store);
        QCOMPARE(result.status, ResolveResult::Status::NeedVersions);
        QCOMPARE(result.missingProjects, QStringList{ "C" });

        store.insert("C", { version("C", "c1") });
        result = resolve({ "A" }, store);
        QCOMPARE(result.status, ResolveResult::Status::Done);
        QCOMPARE(picked(result), (QHash<QString, QString>{ { "A", "a1" }, { "C", "c1" } }));
    }

    void sameIdUnderTwoProvidersIsTwoProjects()
    {
        // "100" is a different mod on each provider, with different dependencies
        const auto modrinth = ModPlatform::ResourceProvider::MODRINTH;
        const auto flame = ModPlatform::ResourceProvider::FLAME;
        ProviderVersions store{
            { modrinth,
              { { "100", { version("100", "m1", { dep("200", DependencyType::REQUIRED) }) } }, { "200", { version("200", "m2") } } } },
            { flame,
              { { "100", { version("100", "f1", { dep("300", DependencyType::REQUIRED) }) } }, { "300", { version("300", "f3") } } } },
        };

        const auto result = resolve(QList<ProjectRef>{ { modrinth, "100" }, { flame, "100" } }, store);

        QCOMPARE(result.status, ResolveResult::Status::Done);
        QSet<QString> got;
        for (const auto& mod : result.mods)
            got.insert(QString("%1/%2/%3").arg(int(mod.provider)).arg(mod.projectId, mod.versionId));
        QCOMPARE(got, (QSet<QString>{ "0/100/m1", "0/200/m2", "1/100/f1", "1/300/f3" }));  // 0 = Modrinth, 1 = CurseForge
        QVERIFY(result.skippedProjects.isEmpty());
    }

    void dependenciesStayWithinTheirProvider()
    {
        // Modrinth's A needs project "7". Only CurseForge has a project "7" in the store, so it must not be used for it.
        const auto modrinth = ModPlatform::ResourceProvider::MODRINTH;
        const auto flame = ModPlatform::ResourceProvider::FLAME;
        ProviderVersions store{
            { modrinth, { { "A", { version("A", "a1", { dep("7", DependencyType::REQUIRED) }) } } } },
            { flame, { { "7", { version("7", "f7") } } } },
        };

        auto result = resolve(QList<ProjectRef>{ { modrinth, "A" } }, store);
        QCOMPARE(result.status, ResolveResult::Status::NeedVersions);
        QCOMPARE(result.missingProjects, QList<ProjectRef>({ { modrinth, "7" } }));

        store[modrinth].insert("7", { version("7", "m7") });
        result = resolve(QList<ProjectRef>{ { modrinth, "A" } }, store);
        QCOMPARE(result.status, ResolveResult::Status::Done);
        QCOMPARE(result.mods.size(), 2);
        for (const auto& mod : result.mods)
            QVERIFY(mod.provider == modrinth);
        QSet<QString> versions;  // the order is the resolver's business, the single provider tests check it
        for (const auto& mod : result.mods)
            versions.insert(mod.versionId);
        QCOMPARE(versions, (QSet<QString>{ "a1", "m7" }));  // not "f7" of the CurseForge project with the same ID
    }

    void asksForTheMissingVersionsOfAllProvidersAtOnce()
    {
        const auto modrinth = ModPlatform::ResourceProvider::MODRINTH;
        const auto flame = ModPlatform::ResourceProvider::FLAME;
        ProviderVersions store{ { modrinth, { { "A", { version("A", "a1") } } } } };

        const auto result = resolve(QList<ProjectRef>{ { modrinth, "A" }, { flame, "5" } }, store);

        QCOMPARE(result.status, ResolveResult::Status::NeedVersions);
        QCOMPARE(result.missingProjects, QList<ProjectRef>({ { flame, "5" } }));
        QVERIFY(result.mods.isEmpty());
    }

    void skippedProjectsKeepTheirProvider()
    {
        const auto flame = ModPlatform::ResourceProvider::FLAME;
        ProviderVersions store{ { flame, { { "5", {} } } } };

        const auto result = resolve(QList<ProjectRef>{ { flame, "5" } }, store);

        QCOMPARE(result.status, ResolveResult::Status::Done);
        QCOMPARE(result.skippedProjects, QList<ProjectRef>({ { flame, "5" } }));
    }

    void leavesOutProjectsWithoutCompatibleVersion()
    {
        VersionStore store{ { "A", {} }, { "B", { version("B", "b1") } } };

        auto result = resolve({ "A", "B" }, store);

        QCOMPARE(result.status, ResolveResult::Status::Done);
        QCOMPARE(picked(result), (QHash<QString, QString>{ { "B", "b1" } }));
        QCOMPARE(result.skippedProjects, QStringList{ "A" });
    }
};

QTEST_GUILESS_MAIN(QuickplayResolverTest)

#include "QuickplayResolver_test.moc"