// SPDX-License-Identifier: GPL-3.0-only

#include "QuickplayResolver.h"

#include <QHash>
#include <QSet>

#include <algorithm>
#include <optional>
#include <utility>

namespace Quickplay {

namespace {

using ModPlatform::DependencyType;
using ModPlatform::IndexedVersion;

enum class Want { Unwanted, Optional, Required };

/// What the resolver currently knows about one project.
struct Entry {
    enum class Kind { Incompatible, Resolved, Unresolved };

    Kind kind = Kind::Unresolved;
    bool wanted = false;                                     // Incompatible: whether the project was wanted
    QString versionId;                                       // Resolved
    Want want = Want::Unwanted;                              // Unresolved
    QSet<QString> incompatible;                              // Unresolved: versions that must not be used
    QSet<QString> preferred;                                 // Unresolved: versions to try first
    std::optional<QList<const IndexedVersion*>> candidates;  // Unresolved: versions left to try, best one last

    static Entry makeIncompatible(bool wanted)
    {
        Entry entry;
        entry.kind = Kind::Incompatible;
        entry.wanted = wanted;
        return entry;
    }

    static Entry makeResolved(const QString& versionId)
    {
        Entry entry;
        entry.kind = Kind::Resolved;
        entry.versionId = versionId;
        return entry;
    }

    static Entry makeUnresolved(Want want, QSet<QString> incompatible = {}, QSet<QString> preferred = {})
    {
        Entry entry;
        entry.want = want;
        entry.incompatible = std::move(incompatible);
        entry.preferred = std::move(preferred);
        return entry;
    }

    bool isWantedUnresolved() const { return kind == Kind::Unresolved && want != Want::Unwanted; }
    bool isRequiredUnresolved() const { return kind == Kind::Unresolved && want == Want::Required; }

    /// Combines what we know with a new requirement. Returns false if they contradict each other.
    bool tryMerge(const Entry& other)
    {
        switch (kind) {
            case Kind::Incompatible:
                switch (other.kind) {
                    case Kind::Incompatible:
                        wanted = wanted || other.wanted;
                        return true;
                    case Kind::Resolved:
                        return false;
                    case Kind::Unresolved:
                        wanted = wanted || other.want != Want::Unwanted;
                        return true;
                }
                break;
            case Kind::Resolved:
                switch (other.kind) {
                    case Kind::Incompatible:
                        return false;
                    case Kind::Resolved:
                        return versionId == other.versionId;
                    case Kind::Unresolved:
                        return !other.incompatible.contains(versionId);
                }
                break;
            case Kind::Unresolved:
                switch (other.kind) {
                    case Kind::Incompatible:
                        if (want == Want::Required)
                            return false;
                        *this = makeIncompatible(other.wanted || want != Want::Unwanted);
                        return true;
                    case Kind::Resolved:
                        if (incompatible.contains(other.versionId))
                            return false;
                        *this = makeResolved(other.versionId);
                        return true;
                    case Kind::Unresolved:
                        want = std::max(want, other.want);
                        incompatible.unite(other.incompatible);
                        preferred.unite(other.preferred);
                        preferred.subtract(incompatible);
                        return true;
                }
                break;
        }
        return false;
    }
};

bool dependsOnVersion(const IndexedVersion& version, const QString& projectId, const QString& versionId)
{
    return std::any_of(version.dependencies.begin(), version.dependencies.end(), [&](const ModPlatform::Dependency& dependency) {
        return dependency.type == DependencyType::REQUIRED && dependency.addonId.toString() == projectId &&
               (dependency.version.isEmpty() || dependency.version == versionId);
    });
}

/// One node of the search. A state is forked for every version we try, so a failed choice can be undone by dropping the fork.
struct State {
    QStringList order;  // insertion order, required projects first once sorted
    QHash<QString, Entry> installs;
    bool initial = true;  // set for new states, the first visit sorts them and checks that all needed versions are known

    void insert(const QString& projectId, Entry entry)
    {
        if (!installs.contains(projectId))
            order.append(projectId);
        installs.insert(projectId, std::move(entry));
    }

    /// Selects the version and records its dependencies. Returns false if that contradicts what is already decided.
    bool tryApplyVersion(const IndexedVersion& candidate)
    {
        insert(candidate.addonId.toString(), Entry::makeResolved(candidate.fileId.toString()));

        for (const auto& dependency : candidate.dependencies) {
            const QString projectId = dependency.addonId.toString();
            if (projectId.isEmpty())
                continue;

            Entry desired;
            switch (dependency.type) {
                case DependencyType::REQUIRED:
                    desired =
                        dependency.version.isEmpty() ? Entry::makeUnresolved(Want::Required) : Entry::makeResolved(dependency.version);
                    break;
                case DependencyType::OPTIONAL:
                    // Not installed on its own, but if the project is installed anyway the recommended version is tried first
                    desired = Entry::makeUnresolved(Want::Unwanted, {},
                                                    dependency.version.isEmpty() ? QSet<QString>{} : QSet<QString>{ dependency.version });
                    break;
                case DependencyType::INCOMPATIBLE:
                    desired = dependency.version.isEmpty() ? Entry::makeIncompatible(false)
                                                           : Entry::makeUnresolved(Want::Unwanted, QSet<QString>{ dependency.version });
                    break;
                default:  // embedded or unknown, nothing to install
                    continue;
            }

            auto existing = installs.find(projectId);
            if (existing == installs.end())
                insert(projectId, std::move(desired));
            else if (!existing->tryMerge(desired))
                return false;
        }
        return true;
    }
};

/// How many of the other wanted projects have a version that needs exactly this version of the project.
int dependentCount(const IndexedVersion& version, const QStringList& wanted, const State& state, const VersionStore& store)
{
    const QString projectId = version.addonId.toString();
    const QString versionId = version.fileId.toString();

    int count = 0;
    for (const auto& other : wanted) {
        if (other == projectId)
            continue;
        const auto otherVersions = store.constFind(other);
        if (otherVersions == store.cend())
            continue;

        const auto otherEntry = state.installs.constFind(other);
        const bool hasExclusions = otherEntry != state.installs.cend() && otherEntry->kind == Entry::Kind::Unresolved;

        const bool anyDepends = std::any_of(otherVersions->begin(), otherVersions->end(), [&](const IndexedVersion& otherVersion) {
            if (hasExclusions && otherEntry->incompatible.contains(otherVersion.fileId.toString()))
                return false;
            return dependsOnVersion(otherVersion, projectId, versionId);
        });
        if (anyDepends)
            count++;
    }
    return count;
}

/// Versions to try for a project. The best one is last, so callers can pop from the back.
QList<const IndexedVersion*> buildCandidates(const QString& projectId,
                                             const Entry& entry,
                                             const State& state,
                                             const QStringList& wanted,
                                             const VersionStore& store)
{
    QList<const IndexedVersion*> candidates;
    const auto versions = store.constFind(projectId);
    if (versions == store.cend())
        return candidates;

    for (const auto& version : *versions) {
        if (!entry.incompatible.contains(version.fileId.toString()))
            candidates.append(&version);
    }

    // The API returns the newest version first and we pop from the back
    std::reverse(candidates.begin(), candidates.end());

    // Preferred versions go last, so they are tried first
    if (!entry.preferred.isEmpty()) {
        std::stable_sort(candidates.begin(), candidates.end(), [&](const IndexedVersion* a, const IndexedVersion* b) {
            return !entry.preferred.contains(a->fileId.toString()) && entry.preferred.contains(b->fileId.toString());
        });
    }

    // Prefer the version most of the other wanted projects can work with. E.g. if Sodium 0.9.2 is needed by [BBE, Iris] but
    // Sodium 0.9.1 by [BBE, Iris, Voxy], taking 0.9.1 keeps Voxy compatible. The sort is stable, so ties keep the order above.
    QHash<const IndexedVersion*, int> counts;
    for (const auto* candidate : candidates)
        counts.insert(candidate, dependentCount(*candidate, wanted, state, store));
    std::stable_sort(candidates.begin(), candidates.end(),
                     [&](const IndexedVersion* a, const IndexedVersion* b) { return counts[a] < counts[b]; });

    return candidates;
}

enum class Step {
    Push,      // `fork` holds a new state to explore
    Pop,       // dead end, go back to the previous state
    Complete,  // everything is decided
};

Step advance(State& current, const QStringList& wanted, const VersionStore& store, State& fork)
{
    for (const QString& projectId : current.order) {
        Entry& entry = current.installs[projectId];
        if (!entry.isWantedUnresolved())
            continue;

        if (!entry.candidates)
            entry.candidates = buildCandidates(projectId, entry, current, wanted, store);

        auto& candidates = *entry.candidates;
        while (!candidates.isEmpty()) {
            const IndexedVersion* candidate = candidates.takeLast();
            if (entry.incompatible.contains(candidate->fileId.toString()))
                continue;
            if (candidate->addonId.toString() != projectId)  // the API returned a version of another project
                continue;

            State attempt = current;
            if (!attempt.tryApplyVersion(*candidate))
                continue;
            attempt.initial = true;
            fork = std::move(attempt);
            return Step::Push;
        }

        // No version works. Dropping a required project makes the state that required it a dead end,
        // while everything else is just left out.
        if (entry.want == Want::Required)
            return Step::Pop;
        entry = Entry::makeIncompatible(true);
    }
    return Step::Complete;
}

ResolveResult collect(const State& state, const QStringList& wanted)
{
    ResolveResult result;
    for (const auto& projectId : state.order) {
        const Entry& entry = state.installs[projectId];
        switch (entry.kind) {
            case Entry::Kind::Resolved:
                result.mods.append({ projectId, entry.versionId, !wanted.contains(projectId) });
                break;
            case Entry::Kind::Incompatible:
                if (entry.wanted)
                    result.skippedProjects.append(projectId);
                break;
            case Entry::Kind::Unresolved:
                if (entry.want != Want::Unwanted) {
                    result.error = QString("Project %1 was left unresolved").arg(projectId);
                    return result;  // status stays Failed
                }
                break;
        }
    }
    result.status = ResolveResult::Status::Done;
    return result;
}

}  // namespace

ResolveResult resolve(const QStringList& wantedProjects, const VersionStore& versions)
{
    State root;
    for (const auto& projectId : wantedProjects)
        root.insert(projectId, Entry::makeUnresolved(Want::Optional));

    QList<State> stack{ root };
    while (!stack.isEmpty()) {
        State& current = stack.last();

        if (current.initial) {
            current.initial = false;

            // Required dependencies first, so contradictions show up early
            std::stable_sort(current.order.begin(), current.order.end(), [&](const QString& a, const QString& b) {
                return current.installs[a].isRequiredUnresolved() && !current.installs[b].isRequiredUnresolved();
            });

            QStringList missing;
            for (const auto& projectId : current.order) {
                if (current.installs[projectId].isWantedUnresolved() && !versions.contains(projectId))
                    missing.append(projectId);
            }
            if (!missing.isEmpty()) {
                missing.sort();
                ResolveResult result;
                result.status = ResolveResult::Status::NeedVersions;
                result.missingProjects = missing;
                return result;
            }
        }

        State fork;
        switch (advance(current, wantedProjects, versions, fork)) {
            case Step::Push:
                stack.push_back(std::move(fork));
                break;
            case Step::Pop:
                stack.pop_back();
                break;
            case Step::Complete:
                return collect(current, wantedProjects);
        }
    }

    ResolveResult result;
    result.error = "No compatible set of mods was found";
    return result;
}

MultiResolveResult resolve(const QList<ProjectRef>& wantedProjects, const ProviderVersions& versions)
{
    QList<ModPlatform::ResourceProvider> providers;
    for (const auto& project : wantedProjects) {
        if (!providers.contains(project.provider))
            providers.append(project.provider);
    }

    MultiResolveResult combined;
    combined.status = ResolveResult::Status::Done;
    for (const auto provider : providers) {
        QStringList wanted;
        for (const auto& project : wantedProjects) {
            if (project.provider == provider)
                wanted.append(project.projectId);
        }

        const auto result = resolve(wanted, versions.value(provider));
        switch (result.status) {
            case ResolveResult::Status::Failed:
                return { ResolveResult::Status::Failed, {}, {}, {}, result.error };
            case ResolveResult::Status::NeedVersions:
                combined.status = ResolveResult::Status::NeedVersions;
                for (const auto& projectId : result.missingProjects)
                    combined.missingProjects.append({ provider, projectId });
                break;
            case ResolveResult::Status::Done:
                for (auto mod : result.mods) {
                    mod.provider = provider;
                    combined.mods.append(mod);
                }
                for (const auto& projectId : result.skippedProjects)
                    combined.skippedProjects.append({ provider, projectId });
                break;
        }
    }

    // Ask for everything that is missing at once, results of the providers that are done are of no use before that.
    if (combined.status == ResolveResult::Status::NeedVersions) {
        combined.mods.clear();
        combined.skippedProjects.clear();
    }
    return combined;
}

}  // namespace Quickplay