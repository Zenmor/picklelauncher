// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QList>
#include <QSet>
#include <QString>
#include <optional>

#include "modplatform/ModIndex.h"

class ResourceAPI;

namespace Quickplay {

/// The API of a provider, the same objects the launcher's own pages use
ResourceAPI& providerApi(ModPlatform::ResourceProvider provider);

/// The Minecraft versions a modpack has files for, read from the answer to the project request. Empty if the answer is not understood.
QSet<QString> packMinecraftVersions(ModPlatform::ResourceProvider provider, const QByteArray& projectJson);

/// The versions in the answer to the version list request, nothing if the answer is not understood. Versions the author does not allow
/// other programs to download have no URL and are left out.
std::optional<QList<ModPlatform::IndexedVersion>> parsePackVersions(ModPlatform::ResourceProvider provider, const QByteArray& json);

/// The version to install for a Minecraft version: the newest release, or the newest of any kind if the pack has no release for it.
std::optional<ModPlatform::IndexedVersion> pickPackVersion(const QList<ModPlatform::IndexedVersion>& versions,
                                                           const QString& minecraftVersion);

}  // namespace Quickplay
