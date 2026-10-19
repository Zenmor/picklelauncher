// SPDX-License-Identifier: GPL-3.0-only

#include "QuickplayPreset.h"

#include <QDebug>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>

#include "Application.h"
#include "FileSystem.h"

namespace Quickplay {

namespace {

// Ordering is important! Projects that are depended on should come first.
QList<Mod> performanceMods()
{
    return {
        { "P7dR8mSH", "Fabric API" },
        { "mOgUt4GM", "Mod Menu" },
        { "AANobbMI", "Sodium" },
        { "gvQqBUqZ", "Lithium" },
        { "VSNURh3q", "c2me" },
        { "p8RJPJIC", "Ixeris" },
        // { "fQEb0iXm", "No Krypton" } is left out on purpose, it is incompatible with e4mc
        { "ONZm0H7Y", "Better Block Entities" },
        { "OnlVIpq5", "Fast Noise" },
        { "x1hIzbuY", "Fast Quit" },
        { "ZP7xHXtw", "Remove Reloading Screen" },
        { "alhWWxax", "Cull Fewer Leaves" },
    };
}

QList<Mod> expandedExtraMods()
{
    return {
        { "YL57xq9U", "Iris" },       { "4das1Fjq", "Flashback" }, { "9eGKb6K1", "Simple Voice Chat" },
        { "w7ThoJFB", "Zoomify" },    { "qANg5Jrr", "e4mc" },      { "QwxR6Gcd", "Debugify" },
        { "M08ruV16", "Bobby" },      { "fxxUqruK", "Voxy" },      { "Kw7Sm3Xf", "Noxesium" },
        { "bEpr0Arc", "Litematica" }, { "TnOXNf5e", "Peek" },      { "EsAfCjCV", "Apple Skin" },
    };
}

// The ID ends up in URLs. CurseForge uses numbers, Modrinth letters and digits.
bool isValidProjectId(ModPlatform::ResourceProvider provider, const QString& id)
{
    static const QRegularExpression modrinth(QStringLiteral("\\A[A-Za-z0-9]+\\z"));
    static const QRegularExpression curseforge(QStringLiteral("\\A[0-9]+\\z"));
    return (provider == ModPlatform::ResourceProvider::FLAME ? curseforge : modrinth).match(id).hasMatch();
}

// Files written before there was a choice of loader have `"fabric": true/false` instead of a "loader" name.
Loader loaderFromJson(const QJsonObject& preset)
{
    if (const auto loader = preset.value("loader"); loader.isString())
        return loaderFromId(loader.toString());  // a loader this version does not know is kept as plain Minecraft, mods stay stored
    return preset.value("fabric").toBool(true) ? Loader::Fabric : Loader::None;
}

QJsonObject modToJson(const Mod& mod)
{
    QJsonObject object{ { "provider", providerId(mod.provider) }, { "id", mod.projectId }, { "name", mod.name } };
    if (!mod.iconUrl.isEmpty())
        object.insert("icon", mod.iconUrl);
    return object;
}

// Files before version 3 have no provider and only know Modrinth. A provider this version does not know gives nothing: taking its IDs
// for Modrinth ones would install the wrong projects.
std::optional<Mod> modFromJson(const QJsonObject& object)
{
    Mod mod{ object.value("id").toString(), object.value("name").toString(), object.value("icon").toString() };
    if (const auto provider = object.value("provider"); !provider.isUndefined()) {
        const auto known = providerFromId(provider.toString());
        if (!known) {
            qWarning() << "Dropping a Quickplay project of the unknown provider" << provider.toString();
            return std::nullopt;
        }
        mod.provider = *known;
    }
    if (!isValidProjectId(mod.provider, mod.projectId))
        return std::nullopt;
    if (mod.name.isEmpty())
        mod.name = mod.projectId;
    return mod;
}

QString presetsFilePath()
{
    return FS::PathCombine(APPLICATION->dataRoot(), "quickplay-presets.json");
}

}  // namespace

QString providerId(ModPlatform::ResourceProvider provider)
{
    return QString::fromLatin1(ModPlatform::ProviderCapabilities::name(provider));
}

std::optional<ModPlatform::ResourceProvider> providerFromId(const QString& id)
{
    for (const auto provider : { ModPlatform::ResourceProvider::MODRINTH, ModPlatform::ResourceProvider::FLAME }) {
        if (providerId(provider) == id)
            return provider;
    }
    return std::nullopt;
}

QList<Mod> Preset::installedMods() const
{
    return loader == Loader::None || kind == Kind::Modpack ? QList<Mod>{} : mods;
}

QList<Preset> defaultPresets()
{
    const auto performance = performanceMods();
    return {
        { "vanilla", "Vanilla", Loader::None, {} },
        { "performance", "Performance", Loader::Fabric, performance },
        { "expanded", "Expanded", Loader::Fabric, performance + expandedExtraMods() },
    };
}

QByteArray presetsToJson(const QList<Preset>& presets)
{
    QJsonArray array;
    for (const auto& preset : presets) {
        QJsonArray mods;
        for (const auto& mod : preset.mods)
            mods.append(modToJson(mod));
        QJsonObject object{ { "id", preset.id }, { "name", preset.name }, { "loader", loaderId(preset.loader) }, { "mods", mods } };
        if (preset.kind == Kind::Modpack) {
            object.insert("kind", "modpack");
            object.insert("pack", modToJson(preset.pack));
        }
        array.append(object);
    }
    return QJsonDocument(QJsonObject{ { "version", 4 }, { "presets", array } }).toJson(QJsonDocument::Indented);
}

QList<Preset> presetsFromJson(const QByteArray& data)
{
    QList<Preset> presets;
    QSet<QString> presetIds;
    for (const auto& presetValue : QJsonDocument::fromJson(data).object().value("presets").toArray()) {
        const auto presetObject = presetValue.toObject();
        Preset preset;
        preset.id = presetObject.value("id").toString();
        preset.name = presetObject.value("name").toString().trimmed();
        preset.loader = loaderFromJson(presetObject);
        if (preset.id.isEmpty() || presetIds.contains(preset.id))
            continue;
        // A kind this version does not know is dropped: its data cannot be interpreted as a mod list.
        if (const auto kind = presetObject.value("kind").toString("mods"); kind == "modpack") {
            const auto pack = modFromJson(presetObject.value("pack").toObject());
            if (!pack)
                continue;
            preset.kind = Kind::Modpack;
            preset.pack = *pack;
        } else if (kind != "mods") {
            qWarning() << "Dropping a Quickplay preset of the unknown kind" << kind;
            continue;
        }
        presetIds.insert(preset.id);
        if (preset.name.isEmpty())
            preset.name = preset.id;

        QSet<QString> projectIds;
        for (const auto& modValue : presetObject.value("mods").toArray()) {
            const auto mod = modFromJson(modValue.toObject());
            if (!mod || projectIds.contains(mod->key()))
                continue;
            projectIds.insert(mod->key());
            preset.mods.append(*mod);
        }
        presets.append(preset);
    }
    return presets;
}

QList<Preset> loadPresets()
{
    const QString path = presetsFilePath();
    QFile file(path);
    if (!file.exists())
        return defaultPresets();

    if (file.open(QIODevice::ReadOnly)) {
        auto presets = presetsFromJson(file.readAll());
        if (!presets.isEmpty())
            return presets;
    }
    qWarning() << "Could not read the Quickplay presets from" << path << ", using the defaults";
    return defaultPresets();
}

bool savePresets(const QList<Preset>& presets)
{
    QSaveFile file(presetsFilePath());
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(presetsToJson(presets));
    return file.commit();
}

}  // namespace Quickplay
