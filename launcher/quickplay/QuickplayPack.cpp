// SPDX-License-Identifier: GPL-3.0-only

#include "QuickplayPack.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

#include "Json.h"
#include "modplatform/flame/FlameAPI.h"
#include "modplatform/modrinth/ModrinthAPI.h"

namespace Quickplay {

ResourceAPI& providerApi(ModPlatform::ResourceProvider provider)
{
    static ModrinthAPI modrinth;
    static FlameAPI flame;
    if (provider == ModPlatform::ResourceProvider::FLAME)
        return flame;
    return modrinth;
}

QSet<QString> packMinecraftVersions(ModPlatform::ResourceProvider provider, const QByteArray& projectJson)
{
    const auto document = QJsonDocument::fromJson(projectJson);
    QSet<QString> versions;

    if (provider == ModPlatform::ResourceProvider::FLAME) {
        // The files of a pack carry the loader next to the Minecraft version in the same list, only the Minecraft ones have a dot.
        for (const auto& index : document.object().value("data").toObject().value("latestFilesIndexes").toArray()) {
            if (const auto version = index.toObject().value("gameVersion").toString(); version.contains('.'))
                versions.insert(version);
        }
        return versions;
    }

    // Modrinth may name a version differently than Minecraft does, the launcher's own pages accept both names.
    for (const auto& value : document.object().value("game_versions").toArray()) {
        versions.insert(value.toString());
        versions.insert(ModrinthAPI::mapMCVersionFromModrinth(value.toString()));
    }
    return versions;
}

std::optional<QList<ModPlatform::IndexedVersion>> parsePackVersions(ModPlatform::ResourceProvider provider, const QByteArray& json)
{
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(json, &error);
    if (error.error != QJsonParseError::NoError)
        return std::nullopt;
    // Modrinth answers with an array, CurseForge with an object that has the array in "data"
    if (!document.isArray() && !document.object().value("data").isArray())
        return std::nullopt;
    const auto array = document.isArray() ? document.array() : document.object().value("data").toArray();

    QList<ModPlatform::IndexedVersion> versions;
    try {
        for (const auto& value : array) {
            auto object = value.toObject();
            auto version = providerApi(provider).loadIndexedPackVersion(object, ModPlatform::ResourceType::Modpack);
            if (!version.fileId.isValid() || version.downloadUrl.isEmpty())
                continue;
            versions.append(version);
        }
    } catch (const Json::JsonException&) {
        return std::nullopt;
    }
    return versions;
}

std::optional<ModPlatform::IndexedVersion> pickPackVersion(const QList<ModPlatform::IndexedVersion>& versions,
                                                           const QString& minecraftVersion)
{
    const auto isRelease = [](const ModPlatform::IndexedVersion& version) {
        return version.version_type == ModPlatform::IndexedVersionType::Release;
    };

    const ModPlatform::IndexedVersion* best = nullptr;
    for (const auto& version : versions) {
        if (version.downloadUrl.isEmpty() || !version.mcVersion.contains(minecraftVersion))
            continue;
        // A release beats anything else, among equals the newest wins. Dates are RFC 3339, so they compare as text.
        if (!best || (isRelease(version) && !isRelease(*best)) || (isRelease(version) == isRelease(*best) && version.date > best->date))
            best = &version;
    }
    return best ? std::optional(*best) : std::nullopt;
}

}  // namespace Quickplay
