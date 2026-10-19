// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QHash>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include "modplatform/ModIndex.h"

namespace Quickplay {

/// Versions per project of one provider, newest first (the order the API returns them), already filtered by game version and loader.
/// A project that is present with an empty list was fetched and has no compatible version.
using VersionStore = QHash<QString, QList<ModPlatform::IndexedVersion>>;

struct ResolvedMod {
    QString projectId;
    QString versionId;
    /// true if the project was pulled in as a dependency instead of being part of the preset
    bool isDependency = false;
    /// Only set by the resolve() overload for several providers
    ModPlatform::ResourceProvider provider = ModPlatform::ResourceProvider::MODRINTH;
};

struct ResolveResult {
    enum class Status {
        Done,          ///< `mods` holds the full set to install
        NeedVersions,  ///< fetch `missingProjects` into the store and call resolve() again
        Failed,        ///< `error` explains why
    };

    Status status = Status::Failed;
    QStringList missingProjects;
    QList<ResolvedMod> mods;
    /// Wanted projects without a version that works with the rest of the set
    QStringList skippedProjects;
    QString error;
};

/// Picks one version per project so that the whole set is compatible: required dependencies are pulled in, incompatibilities are
/// respected and versions the other mods depend on are preferred. Projects are handled in the given order, earlier ones win conflicts.
///
/// Pure function of its inputs. It never fetches anything: whenever it needs versions of a project that is not in `versions` it
/// returns NeedVersions, the caller fetches them and calls it again (restarting is cheap and gives the same result).
/// Based on the mod set resolver of Pandora Launcher.
ResolveResult resolve(const QStringList& wantedProjects, const VersionStore& versions);

/// A project of a provider. The same project ID under two providers is two different projects.
struct ProjectRef {
    ModPlatform::ResourceProvider provider = ModPlatform::ResourceProvider::MODRINTH;
    QString projectId;

    bool operator==(const ProjectRef&) const = default;
};

using ProviderVersions = QMap<ModPlatform::ResourceProvider, VersionStore>;

struct MultiResolveResult {
    ResolveResult::Status status = ResolveResult::Status::Failed;
    QList<ProjectRef> missingProjects;
    QList<ResolvedMod> mods;  ///< with `provider` set
    QList<ProjectRef> skippedProjects;
    QString error;
};

/// resolve() for projects of several providers. Dependencies and incompatibilities only ever refer to projects of the same provider,
/// so every provider is resolved on its own and a project ID is looked up only in the versions of its own provider.
/// `mods` are listed provider by provider, in the order the providers first appear in `wantedProjects`.
MultiResolveResult resolve(const QList<ProjectRef>& wantedProjects, const ProviderVersions& versions);

}  // namespace Quickplay