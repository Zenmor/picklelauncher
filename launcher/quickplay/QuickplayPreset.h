// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

#include "modplatform/ModIndex.h"
#include "quickplay/QuickplayLoader.h"

namespace Quickplay {

/// The id of a provider as it is stored in presets.
QString providerId(ModPlatform::ResourceProvider provider);
std::optional<ModPlatform::ResourceProvider> providerFromId(const QString& id);

/// A project of a provider. The same project ID under two providers is two different projects.
struct Mod {
    QString projectId;  ///< ID of the provider, not the slug (the resolver matches dependencies by ID)
    QString name;       ///< Only shown to the user
    QString iconUrl;    ///< Only shown to the user, empty if not known yet
    ModPlatform::ResourceProvider provider = ModPlatform::ResourceProvider::MODRINTH;

    /// Identifies the project among all providers, e.g. for sets.
    QString key() const { return providerId(provider) + ':' + projectId; }
};

/// A preset is either a list of mods on top of plain Minecraft, or one modpack.
enum class Kind { Mods, Modpack };

struct Preset {
    QString id;                      ///< Stable identifier, this is what the settings store
    QString name;                    ///< Shown to the user
    Loader loader = Loader::Fabric;  ///< Without a loader a preset is plain Minecraft and its mods are ignored
    QList<Mod> mods;                 ///< In dependency order: projects that others depend on first, earlier ones win conflicts
    Kind kind = Kind::Mods;
    Mod pack;  ///< The modpack project of a Modpack preset. Loader and mods are not used then; the version is chosen per Minecraft version

    /// The mods to install. Empty without a loader, and for a modpack.
    QList<Mod> installedMods() const;
};

/// Vanilla, Performance and Expanded.
QList<Preset> defaultPresets();

QByteArray presetsToJson(const QList<Preset>& presets);
/// Invalid entries are dropped, so the result is empty if nothing usable is in the data.
QList<Preset> presetsFromJson(const QByteArray& data);

/// The presets the user saved in the data folder, or the defaults if there are none. Never empty.
QList<Preset> loadPresets();
bool savePresets(const QList<Preset>& presets);

}  // namespace Quickplay
