// SPDX-License-Identifier: GPL-3.0-only

#include "QuickplayLoader.h"

#include <QObject>
#include <algorithm>

#include "Application.h"
#include "meta/Index.h"
#include "meta/JsonFormat.h"
#include "meta/Version.h"
#include "modplatform/ModIndex.h"

namespace Quickplay {

const QString IntermediaryUid = QStringLiteral("net.fabricmc.intermediary");

namespace {

const QString MinecraftUid = QStringLiteral("net.minecraft");

ModPlatform::ModLoaderType platformType(Loader loader)
{
    switch (loader) {
        case Loader::Fabric:
            return ModPlatform::Fabric;
        case Loader::Quilt:
            return ModPlatform::Quilt;
        case Loader::Forge:
            return ModPlatform::Forge;
        case Loader::NeoForge:
            return ModPlatform::NeoForge;
        case Loader::LiteLoader:
            return ModPlatform::LiteLoader;
        case Loader::None:
            break;
    }
    return ModPlatform::ModLoaderType(0);
}

// A release beats anything else, otherwise the newer one wins (the same rule as Meta::VersionList::getLatestForParent).
bool isBetter(const Meta::Version::Ptr& candidate, const Meta::Version::Ptr& current)
{
    if (!current)
        return true;
    const bool candidateRelease = candidate->type() == "release";
    if (candidateRelease != (current->type() == "release"))
        return candidateRelease;
    return candidate->rawTime() > current->rawTime();
}

}  // namespace

QList<Loader> modLoaders()
{
    return { Loader::Fabric, Loader::Quilt, Loader::Forge, Loader::NeoForge, Loader::LiteLoader };
}

QString loaderId(Loader loader)
{
    return loader == Loader::None ? QStringLiteral("none") : ModPlatform::getModLoaderAsString(platformType(loader));
}

Loader loaderFromId(const QString& id, Loader fallback)
{
    if (id == "none")
        return Loader::None;
    for (const auto loader : modLoaders()) {
        if (id == loaderId(loader))
            return loader;
    }
    return fallback;
}

QString loaderName(Loader loader)
{
    switch (loader) {
        case Loader::Fabric:
            return QObject::tr("Fabric");
        case Loader::Quilt:
            return QObject::tr("Quilt");
        case Loader::Forge:
            return QObject::tr("Forge");
        case Loader::NeoForge:
            return QObject::tr("NeoForge");
        case Loader::LiteLoader:
            return QObject::tr("LiteLoader");
        case Loader::None:
            break;
    }
    return QObject::tr("None (vanilla)");
}

QString loaderUid(Loader loader)
{
    switch (loader) {
        case Loader::Fabric:
            return QStringLiteral("net.fabricmc.fabric-loader");
        case Loader::Quilt:
            return QStringLiteral("org.quiltmc.quilt-loader");
        case Loader::Forge:
            return QStringLiteral("net.minecraftforge");
        case Loader::NeoForge:
            return QStringLiteral("net.neoforged");
        case Loader::LiteLoader:
            return QStringLiteral("com.mumfrey.liteloader");
        case Loader::None:
            break;
    }
    return {};
}

bool usesIntermediary(Loader loader)
{
    return loader == Loader::Fabric || loader == Loader::Quilt;
}

QStringList modrinthLoaders(Loader loader, const QString& minecraftVersion)
{
    if (loader == Loader::None)
        return {};

    QStringList names{ ModPlatform::getModLoaderAsString(platformType(loader)) };
    if (loader == Loader::Quilt)
        names.append(ModPlatform::getModLoaderAsString(ModPlatform::Fabric));
    if (loader == Loader::NeoForge && minecraftVersion == "1.20.1")
        names.append(ModPlatform::getModLoaderAsString(ModPlatform::Forge));
    return names;
}

ModPlatform::ModLoaderTypes modLoaderTypes(Loader loader, const QString& minecraftVersion)
{
    ModPlatform::ModLoaderTypes types;
    for (const auto& name : modrinthLoaders(loader, minecraftVersion))
        types |= ModPlatform::getModLoaderFromString(name);
    return types;
}

QList<Meta::VersionList::Ptr> loaderLists(Loader loader)
{
    QList<Meta::VersionList::Ptr> lists;
    if (loader == Loader::None)
        return lists;

    auto* index = APPLICATION->metadataIndex();
    lists.append(index->get(loaderUid(loader)));
    if (usesIntermediary(loader))
        lists.append(index->get(IntermediaryUid));
    return lists;
}

QSet<QString> supportedMinecraftVersions(Loader loader)
{
    return supportedMinecraftVersions(loader, loaderLists(loader));
}

QSet<QString> supportedMinecraftVersions(Loader loader, const QList<Meta::VersionList::Ptr>& lists)
{
    QSet<QString> supported;
    if (lists.isEmpty() || std::any_of(lists.begin(), lists.end(), [](const auto& list) { return !list; }))
        return supported;

    if (usesIntermediary(loader)) {
        // Loader versions do not name a Minecraft version, but each intermediary mapping is made for exactly one.
        for (const auto& version : lists.at(1)->versions())
            supported.insert(version->version());
        return supported;
    }

    // Every loader version requires exactly one Minecraft version.
    for (const auto& version : lists.at(0)->versions()) {
        for (const auto& require : version->requiredSet()) {
            if (require.uid == MinecraftUid)
                supported.insert(require.equalsVersion);
        }
    }
    return supported;
}

bool supportsMinecraft(Loader loader, const QString& minecraftVersion)
{
    return loader == Loader::None || supportedMinecraftVersions(loader).contains(minecraftVersion);
}

BaseVersion::Ptr pickLoaderVersion(Loader loader, const QString& minecraftVersion)
{
    return pickLoaderVersion(loader, minecraftVersion, loaderLists(loader));
}

BaseVersion::Ptr pickLoaderVersion(Loader loader, const QString& minecraftVersion, const QList<Meta::VersionList::Ptr>& lists)
{
    if (lists.isEmpty() || !lists.at(0))
        return nullptr;
    const auto& list = lists.at(0);

    if (!usesIntermediary(loader)) {
        // These loader versions belong to one Minecraft version, so only that one's versions are candidates.
        if (const auto recommended = list->getRecommendedForParent(MinecraftUid, minecraftVersion))
            return recommended;
        return list->getLatestForParent(MinecraftUid, minecraftVersion);
    }

    if (const auto recommended = list->getRecommended())
        return recommended;
    Meta::Version::Ptr best;
    for (const auto& version : list->versions()) {
        if (isBetter(version, best))
            best = version;
    }
    return best;
}

}  // namespace Quickplay
