// SPDX-License-Identifier: GPL-3.0-only

#include <QTest>

#include "quickplay/QuickplayPreset.h"

using namespace Quickplay;

class QuickplayPresetTest : public QObject {
    Q_OBJECT

    static bool same(const QList<Preset>& a, const QList<Preset>& b)
    {
        if (a.size() != b.size())
            return false;
        for (int i = 0; i < a.size(); i++) {
            if (a[i].id != b[i].id || a[i].name != b[i].name || a[i].loader != b[i].loader || a[i].mods.size() != b[i].mods.size())
                return false;
            for (int j = 0; j < a[i].mods.size(); j++) {
                if (a[i].mods[j].projectId != b[i].mods[j].projectId || a[i].mods[j].name != b[i].mods[j].name ||
                    a[i].mods[j].iconUrl != b[i].mods[j].iconUrl || a[i].mods[j].provider != b[i].mods[j].provider)
                    return false;
            }
        }
        return true;
    }

    static QStringList ids(const Preset& preset)
    {
        QStringList result;
        for (const auto& mod : preset.installedMods())
            result.append(mod.projectId);
        return result;
    }

   private slots:
    void defaults_areConsistent()
    {
        const auto presets = defaultPresets();
        QCOMPARE(presets.size(), 3);

        QVERIFY(presets[0].loader == Loader::None);
        QVERIFY(presets[0].installedMods().isEmpty());
        QVERIFY(presets[1].loader == Loader::Fabric);
        QVERIFY(presets[2].loader == Loader::Fabric);

        const auto performance = ids(presets[1]);
        const auto expanded = ids(presets[2]);
        QVERIFY(!performance.isEmpty());
        QVERIFY(expanded.size() > performance.size());
        QCOMPARE(expanded.mid(0, performance.size()), performance);  // Expanded is Performance plus extras

        for (const auto& preset : presets) {
            const auto projects = ids(preset);
            QCOMPARE(QSet<QString>(projects.begin(), projects.end()).size(), projects.size());  // no project twice
            for (const auto& mod : preset.mods)
                QVERIFY(mod.provider == ModPlatform::ResourceProvider::MODRINTH);  // the defaults are Modrinth IDs
        }
    }

    void json_roundTrip()
    {
        auto presets = defaultPresets();
        presets.append({ "custom", "My preset", Loader::NeoForge, { { "AANobbMI", "Sodium" } } });
        QVERIFY(same(presetsFromJson(presetsToJson(presets)), presets));
    }

    void json_roundTripKeepsIconUrl()
    {
        QList<Preset> presets{ { "custom",
                                 "P",
                                 Loader::Fabric,
                                 { { "AANobbMI", "Sodium", "https://cdn.modrinth.com/icon.png" }, { "gvQqBUqZ", "Lithium" } } } };
        const auto loaded = presetsFromJson(presetsToJson(presets));
        QVERIFY(same(loaded, presets));
        QCOMPARE(loaded.first().mods.first().iconUrl, QString("https://cdn.modrinth.com/icon.png"));
        QVERIFY(loaded.first().mods.last().iconUrl.isEmpty());
    }

    void json_roundTripKeepsEveryLoader()
    {
        QList<Preset> presets;
        for (const auto loader : { Loader::None, Loader::Fabric, Loader::Quilt, Loader::Forge, Loader::NeoForge, Loader::LiteLoader })
            presets.append({ loaderId(loader), "P", loader, { { "AANobbMI", "Sodium" } } });
        const auto loaded = presetsFromJson(presetsToJson(presets));
        QVERIFY(same(loaded, presets));
        QCOMPARE(loaded.size(), 6);
    }

    void json_oldFilesMapFabricFlagToLoader()
    {
        // Files from before the loader choice: "fabric" is a bool, and Fabric is the default when it is missing.
        const auto presets = presetsFromJson(R"({"version": 1, "presets": [
            {"id": "a", "name": "A", "fabric": true, "mods": [{"id": "AANobbMI", "name": "Sodium"}]},
            {"id": "b", "name": "B", "fabric": false, "mods": [{"id": "AANobbMI", "name": "Sodium"}]},
            {"id": "c", "name": "C"}
        ]})");
        QCOMPARE(presets.size(), 3);
        QVERIFY(presets[0].loader == Loader::Fabric);
        QVERIFY(presets[1].loader == Loader::None);
        QCOMPARE(presets[1].mods.size(), 1);  // the mods of a preset without loader stay stored
        QVERIFY(presets[2].loader == Loader::Fabric);

        // Saving writes the new format, and reading that again gives the same presets.
        const auto saved = presetsToJson(presets);
        QVERIFY(!saved.contains("\"fabric\":"));  // the old key, "loader": "fabric" is fine
        QVERIFY(same(presetsFromJson(saved), presets));
    }

    void json_loaderWinsOverOldFlag()
    {
        const auto presets = presetsFromJson(R"({"presets": [
            {"id": "a", "loader": "forge", "fabric": false},
            {"id": "b", "loader": "none", "fabric": true},
            {"id": "c", "loader": "somethingnew"}
        ]})");
        QCOMPARE(presets.size(), 3);
        QVERIFY(presets[0].loader == Loader::Forge);
        QVERIFY(presets[1].loader == Loader::None);
        QVERIFY(presets[2].loader == Loader::None);  // unknown to this version, the preset stays usable as plain Minecraft
    }

    void json_roundTripKeepsProvider()
    {
        Mod curseforge{ "394468", "Sodium", "https://media.forgecdn.net/icon.png" };
        curseforge.provider = ModPlatform::ResourceProvider::FLAME;
        const QList<Preset> presets{ { "p", "P", Loader::Fabric, { { "AANobbMI", "Sodium" }, curseforge } } };

        const auto data = presetsToJson(presets);
        QVERIFY(data.contains("\"version\": 4"));
        QVERIFY(data.contains("\"curseforge\""));
        const auto loaded = presetsFromJson(data);
        QVERIFY(same(loaded, presets));
        QVERIFY(loaded[0].mods[0].provider == ModPlatform::ResourceProvider::MODRINTH);
        QVERIFY(loaded[0].mods[1].provider == ModPlatform::ResourceProvider::FLAME);
    }

    void json_version2FilesMeanModrinth()
    {
        const auto presets = presetsFromJson(R"({"version": 2, "presets": [
            {"id": "a", "name": "A", "loader": "fabric", "mods": [{"id": "AANobbMI", "name": "Sodium", "icon": "icon.png"}]}
        ]})");
        QCOMPARE(presets.size(), 1);
        QCOMPARE(presets[0].mods.size(), 1);
        QVERIFY(presets[0].mods[0].provider == ModPlatform::ResourceProvider::MODRINTH);
        QCOMPARE(presets[0].mods[0].iconUrl, QString("icon.png"));  // no "//" here: moc takes it for a comment in a raw string
        QVERIFY(presetsToJson(presets).contains("\"modrinth\""));   // saving writes the provider
    }

    void json_sameIdUnderTwoProvidersIsNoDuplicate()
    {
        const auto presets = presetsFromJson(R"({"version": 3, "presets": [{"id": "a", "mods": [
            {"provider": "modrinth", "id": "123", "name": "From Modrinth"},
            {"provider": "curseforge", "id": "123", "name": "From CurseForge"},
            {"provider": "curseforge", "id": "123", "name": "CurseForge twice"},
            {"id": "123", "name": "Modrinth twice"}
        ]}]})");
        QCOMPARE(presets.size(), 1);
        QCOMPARE(presets[0].mods.size(), 2);
        QCOMPARE(presets[0].mods[0].name, QString("From Modrinth"));
        QCOMPARE(presets[0].mods[1].name, QString("From CurseForge"));
        QVERIFY(presets[0].mods[0].key() != presets[0].mods[1].key());
    }

    void json_dropsUnknownProvidersAndBadCurseForgeIds()
    {
        const auto presets = presetsFromJson(R"({"version": 3, "presets": [{"id": "a", "mods": [
            {"provider": "somethingnew", "id": "AANobbMI", "name": "Unknown provider"},
            {"provider": 5, "id": "AANobbMI", "name": "Provider is no string"},
            {"provider": "curseforge", "id": "AANobbMI", "name": "CurseForge IDs are numbers"},
            {"provider": "curseforge", "id": "394468", "name": "Sodium"},
            {"provider": "modrinth", "id": "gvQqBUqZ", "name": "Lithium"}
        ]}]})");
        QCOMPARE(presets.size(), 1);
        QCOMPARE(presets[0].mods.size(), 2);
        QCOMPARE(presets[0].mods[0].projectId, QString("394468"));
        QCOMPARE(presets[0].mods[1].projectId, QString("gvQqBUqZ"));
    }

    void provider_idsAreStable()
    {
        QCOMPARE(providerId(ModPlatform::ResourceProvider::MODRINTH), QString("modrinth"));
        QCOMPARE(providerId(ModPlatform::ResourceProvider::FLAME), QString("curseforge"));
        QVERIFY(providerFromId("modrinth") == ModPlatform::ResourceProvider::MODRINTH);
        QVERIFY(providerFromId("curseforge") == ModPlatform::ResourceProvider::FLAME);
        QVERIFY(!providerFromId("flame"));
        QVERIFY(!providerFromId(""));
    }

    void loader_idsAreStable()
    {
        QCOMPARE(loaderId(Loader::None), QString("none"));
        QCOMPARE(loaderId(Loader::Fabric), QString("fabric"));
        QCOMPARE(loaderId(Loader::Quilt), QString("quilt"));
        QCOMPARE(loaderId(Loader::Forge), QString("forge"));
        QCOMPARE(loaderId(Loader::NeoForge), QString("neoforge"));
        QCOMPARE(loaderId(Loader::LiteLoader), QString("liteloader"));
        for (const auto loader : modLoaders())
            QVERIFY(loaderFromId(loaderId(loader)) == loader);
        QVERIFY(loaderFromId("", Loader::Forge) == Loader::Forge);
    }

    void loader_modrinthLoaders()
    {
        QVERIFY(modrinthLoaders(Loader::None, "1.21").isEmpty());
        QCOMPARE(modrinthLoaders(Loader::Fabric, "1.21"), QStringList({ "fabric" }));
        QCOMPARE(modrinthLoaders(Loader::Quilt, "1.21"), QStringList({ "quilt", "fabric" }));  // Quilt runs Fabric mods
        QCOMPARE(modrinthLoaders(Loader::Forge, "1.20.1"), QStringList({ "forge" }));
        QCOMPARE(modrinthLoaders(Loader::NeoForge, "1.21"), QStringList({ "neoforge" }));
        QCOMPARE(modrinthLoaders(Loader::NeoForge, "1.20.1"), QStringList({ "neoforge", "forge" }));
        QCOMPARE(modrinthLoaders(Loader::NeoForge), QStringList({ "neoforge" }));  // no version known
        QCOMPARE(modrinthLoaders(Loader::LiteLoader, "1.12.2"), QStringList({ "liteloader" }));
    }

    void loader_modLoaderTypes()
    {
        using ModPlatform::ModLoaderTypes;
        QCOMPARE(modLoaderTypes(Loader::None, "1.21"), ModLoaderTypes());
        QCOMPARE(modLoaderTypes(Loader::Fabric, "1.21"), ModLoaderTypes(ModPlatform::Fabric));
        QCOMPARE(modLoaderTypes(Loader::Quilt, "1.21"), ModLoaderTypes(ModPlatform::Quilt | ModPlatform::Fabric));
        QCOMPARE(modLoaderTypes(Loader::NeoForge, "1.20.1"), ModLoaderTypes(ModPlatform::NeoForge | ModPlatform::Forge));
        QCOMPARE(modLoaderTypes(Loader::LiteLoader, "1.12.2"), ModLoaderTypes(ModPlatform::LiteLoader));
    }

    void loader_uidsAndIntermediary()
    {
        QVERIFY(loaderUid(Loader::None).isEmpty());
        QCOMPARE(loaderUid(Loader::Fabric), QString("net.fabricmc.fabric-loader"));
        QCOMPARE(loaderUid(Loader::Quilt), QString("org.quiltmc.quilt-loader"));
        QCOMPARE(loaderUid(Loader::Forge), QString("net.minecraftforge"));
        QCOMPARE(loaderUid(Loader::NeoForge), QString("net.neoforged"));
        QCOMPARE(loaderUid(Loader::LiteLoader), QString("com.mumfrey.liteloader"));
        QVERIFY(usesIntermediary(Loader::Fabric));
        QVERIFY(usesIntermediary(Loader::Quilt));
        QVERIFY(!usesIntermediary(Loader::Forge));
        QVERIFY(!usesIntermediary(Loader::NeoForge));
    }

    void json_withoutLoaderKeepsModsButIgnoresThem()
    {
        const Preset preset{ "p", "P", Loader::None, { { "AANobbMI", "Sodium" } } };
        const auto loaded = presetsFromJson(presetsToJson({ preset }));
        QCOMPARE(loaded.size(), 1);
        QCOMPARE(loaded[0].mods.size(), 1);
        QVERIFY(loaded[0].installedMods().isEmpty());
    }

    void json_rejectsUnusableData_data()
    {
        QTest::addColumn<QByteArray>("data");
        QTest::newRow("empty") << QByteArray();
        QTest::newRow("not json") << QByteArray("presets");
        QTest::newRow("wrong type") << QByteArray(R"({"presets": "x"})");
        QTest::newRow("no presets") << QByteArray(R"({"presets": []})");
        QTest::newRow("no id") << QByteArray(R"({"presets": [{"name": "A"}]})");
    }
    void json_rejectsUnusableData()
    {
        QFETCH(QByteArray, data);
        QVERIFY(presetsFromJson(data).isEmpty());
    }

    void json_dropsInvalidEntries()
    {
        const auto presets = presetsFromJson(R"({"presets": [
            {"id": "a", "mods": [
                {"id": "AANobbMI", "name": "Sodium"},
                {"id": "AANobbMI", "name": "Sodium again"},
                {"id": "../evil", "name": "Bad id"},
                {"id": "", "name": "No id"},
                {"id": "gvQqBUqZ"}
            ]},
            {"id": "a", "name": "Same id twice"}
        ]})");

        QCOMPARE(presets.size(), 1);
        QCOMPARE(presets[0].name, QString("a"));       // falls back to the id
        QVERIFY(presets[0].loader == Loader::Fabric);  // Fabric unless said otherwise
        QCOMPARE(ids(presets[0]), QStringList({ "AANobbMI", "gvQqBUqZ" }));
        QCOMPARE(presets[0].mods[1].name, QString("gvQqBUqZ"));
    }

    void json_modpackRoundTrip()
    {
        Preset preset{ "fo", "Fabulously Optimized", Loader::None, {} };
        preset.kind = Kind::Modpack;
        preset.pack = { "1KVo5zza", "Fabulously Optimized", "https:/x/icon.png", ModPlatform::ResourceProvider::MODRINTH };
        Preset curse{ "cf", "CF", Loader::None, {} };
        curse.kind = Kind::Modpack;
        curse.pack = { "396246", "Pack", {}, ModPlatform::ResourceProvider::FLAME };

        const auto loaded = presetsFromJson(presetsToJson({ preset, curse }));
        QCOMPARE(loaded.size(), 2);
        for (int i = 0; i < 2; i++) {
            const auto& expected = i == 0 ? preset : curse;
            QVERIFY(loaded[i].kind == Kind::Modpack);
            QCOMPARE(loaded[i].pack.key(), expected.pack.key());
            QCOMPARE(loaded[i].pack.name, expected.pack.name);
            QCOMPARE(loaded[i].pack.iconUrl, expected.pack.iconUrl);
            QVERIFY(loaded[i].installedMods().isEmpty());
        }
    }

    void json_modsPresetsStayModsAndVersionThreeLoads()
    {
        const auto presets = presetsFromJson(R"({"version": 3, "presets": [
            {"id": "a", "loader": "fabric", "mods": [{"provider": "curseforge", "id": "306612", "name": "Fabric API"}]}
        ]})");
        QCOMPARE(presets.size(), 1);
        QVERIFY(presets[0].kind == Kind::Mods);
        QCOMPARE(presets[0].mods.size(), 1);
        QVERIFY(!QString::fromUtf8(presetsToJson(presets)).contains("\"kind\""));  // mod presets are written as before
    }

    void json_dropsUnusableModpackPresets_data()
    {
        QTest::addColumn<QByteArray>("data");
        QTest::newRow("no pack") << QByteArray(R"({"presets": [{"id": "a", "kind": "modpack"}]})");
        QTest::newRow("bad id") << QByteArray(R"({"presets": [{"id": "a", "kind": "modpack", "pack": {"id": "../x"}}]})");
        QTest::newRow("unknown provider") << QByteArray(
            R"({"presets": [{"id": "a", "kind": "modpack", "pack": {"provider": "ftb", "id": "1"}}]})");
        QTest::newRow("unknown kind") << QByteArray(R"({"presets": [{"id": "a", "kind": "world", "mods": []}]})");
    }
    void json_dropsUnusableModpackPresets()
    {
        QFETCH(QByteArray, data);
        QVERIFY(presetsFromJson(data).isEmpty());
    }
};

QTEST_GUILESS_MAIN(QuickplayPresetTest)

#include "QuickplayPreset_test.moc"
