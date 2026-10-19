// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

#include "BaseVersion.h"
#include "meta/VersionList.h"
#include "modplatform/ModIndex.h"

namespace Quickplay {

/// The mod loaders of the "Install Loader" dialog (Component::KNOWN_MODLOADERS), or none for plain Minecraft.
enum class Loader { None, Fabric, Quilt, Forge, NeoForge, LiteLoader };

/// Every loader except None, in the order the user sees them.
QList<Loader> modLoaders();

/// Stable name stored in the presets file. It is the loader name Modrinth uses ("fabric", "neoforge", ...), "none" for None.
QString loaderId(Loader loader);
/// Unknown ids give `fallback`.
Loader loaderFromId(const QString& id, Loader fallback = Loader::None);
/// Shown to the user.
QString loaderName(Loader loader);
/// UID of the loader component in the instance and in the version metadata. Empty for None.
QString loaderUid(Loader loader);

/// Fabric and Quilt are matched to Minecraft by the intermediary mappings, not by the loader version.
bool usesIntermediary(Loader loader);
extern const QString IntermediaryUid;

/// Modrinth loader names to search mod versions for, empty for None. Quilt also runs Fabric mods, and NeoForge 1.20.1 also Forge mods,
/// as in PackProfile::getSupportedModLoaders(). `minecraftVersion` may be empty when it is not known.
QStringList modrinthLoaders(Loader loader, const QString& minecraftVersion = QString());
/// The same loaders as flags, for the search and version requests of the platform APIs. Empty for None.
ModPlatform::ModLoaderTypes modLoaderTypes(Loader loader, const QString& minecraftVersion = QString());

/// The metadata lists that tell which Minecraft versions a loader supports and which versions it has. Empty for None, may contain
/// null entries if the metadata does not know a list. They must be loaded before the functions below are used.
QList<Meta::VersionList::Ptr> loaderLists(Loader loader);

/// Whether the loader can be installed for a Minecraft version. Always true for None.
bool supportsMinecraft(Loader loader, const QString& minecraftVersion);
/// All Minecraft versions the loader supports, for filtering long lists. Empty for None.
QSet<QString> supportedMinecraftVersions(Loader loader);
/// The same for the given lists, in the order of loaderLists().
QSet<QString> supportedMinecraftVersions(Loader loader, const QList<Meta::VersionList::Ptr>& lists);

/// The loader version to install for a Minecraft version: the recommended one, otherwise the newest release, otherwise the newest.
/// Null if there is none.
BaseVersion::Ptr pickLoaderVersion(Loader loader, const QString& minecraftVersion);
/// The same for the given lists, in the order of loaderLists().
BaseVersion::Ptr pickLoaderVersion(Loader loader, const QString& minecraftVersion, const QList<Meta::VersionList::Ptr>& lists);

}  // namespace Quickplay
