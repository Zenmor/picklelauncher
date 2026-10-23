// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackPlan.h"

#include <QSet>
#include <algorithm>
#include <optional>

#include "SharedPackPath.h"

namespace SharedPack {

namespace {

struct Decision {
    Action action;
    QString track;  ///< what State::installed records for the path afterwards, empty = stop tracking it
};

Decision decide(ActionKind kind, const QString& path, const QString& sha512, Why why, const QString& track)
{
    return { Action{ kind, path, sha512, why }, track };
}

// A file the new manifest has. \a oldSha is what the state recorded for it, empty when the pack did not have it before.
Decision decideWanted(FileClass fileClass, const File& file, const QString& oldSha, const DiskEntry& disk)
{
    const QString& path = file.path;
    const QString& now = file.sha512;

    if (fileClass == FileClass::Excluded)
        return decide(ActionKind::Skip, path, now, Why::Excluded, {});
    if (disk.kind == DiskKind::Unsafe)
        return decide(ActionKind::Skip, path, now, Why::UnsafeOnDisk, oldSha);

    const bool missing = disk.kind == DiskKind::Missing;
    if (!missing && disk.sha512 == now)
        return decide(ActionKind::Keep, path, now, Why::AlreadyCurrent, now);

    if (fileClass == FileClass::SeedOnly) {
        if (missing)
            return decide(ActionKind::Write, path, now, Why::SeedMissing, now);
        return decide(ActionKind::Keep, path, now, Why::SeedExists, {});
    }
    if (fileClass == FileClass::PackWins)
        return decide(ActionKind::Write, path, now, missing ? Why::NewFile : Why::PackChanged, now);

    // Merge: what the member did with the file decides.
    if (missing) {
        if (oldSha.isEmpty())
            return decide(ActionKind::Write, path, now, Why::NewFile, now);
        if (oldSha == now)
            return decide(ActionKind::Keep, path, now, Why::UserDeleted, oldSha);
        return decide(ActionKind::Write, path, now, Why::RestoredChanged, now);
    }
    if (oldSha.isEmpty())
        return decide(ActionKind::PackNew, path, now, Why::Conflict, now);  // the member had a file of that name before the pack did
    if (disk.sha512 == oldSha)
        return decide(ActionKind::Write, path, now, Why::PackChanged, now);  // not edited: the pack's change applies
    if (oldSha == now)
        return decide(ActionKind::Keep, path, now, Why::UserEdited, oldSha);  // edited, and the pack did not change it
    // Both changed. Record the pack's version as the base: the member's file is now "theirs" against it, so the next update that does not
    // touch the file again leaves it alone instead of warning every time.
    return decide(ActionKind::PackNew, path, now, Why::Conflict, now);
}

// A file the state has and the new manifest dropped. Nothing is tracked afterwards. Nothing to do when it is already gone.
std::optional<Decision> decideRemoved(FileClass fileClass, const QString& path, const QString& oldSha, const DiskEntry& disk,
                                      const PathSet& released)
{
    if (fileClass == FileClass::Excluded)
        return decide(ActionKind::Skip, path, oldSha, Why::Excluded, {});
    if (disk.kind == DiskKind::Missing)
        return std::nullopt;
    if (disk.kind == DiskKind::Unsafe)
        return decide(ActionKind::Skip, path, oldSha, Why::UnsafeOnDisk, {});
    // The owner still has the file but stopped publishing it: that is not "the pack dropped it", so it is never deleted.
    if (released.covers(path))
        return decide(ActionKind::Keep, path, oldSha, Why::Released, {});
    if (fileClass == FileClass::SeedOnly)
        return decide(ActionKind::Keep, path, oldSha, Why::SeedExists, {});
    if (disk.sha512 == oldSha)
        return decide(ActionKind::Delete, path, oldSha, Why::PackRemoved, {});
    return decide(ActionKind::Keep, path, oldSha, Why::RemovedButEdited, {});
}

}  // namespace

Plan planUpdate(const State& state, const Manifest& next, const DiskState& disk)
{
    Plan plan;
    plan.revision = next.revision;
    plan.changelog = next.changelog;
    if (state.role == Role::Owner) {
        plan.refusal = Plan::Refusal::OwnerRole;
        return plan;
    }
    if (next.packId != state.packId) {
        plan.refusal = Plan::Refusal::WrongPack;
        return plan;
    }
    if (next.revision < state.revision) {
        plan.refusal = Plan::Refusal::Backwards;
        return plan;
    }

    struct Old {
        QString path;
        QString sha512;
    };
    QHash<QString, Old> old;  // by lower-case path
    for (auto it = state.installed.cbegin(); it != state.installed.cend(); ++it)
        old.insert(it.key().toLower(), Old{ it.key(), it.value() });

    QList<Action> deletes;
    QList<Action> rest;
    auto add = [&](const Decision& decision) {
        if (!decision.track.isEmpty())
            plan.installed.insert(decision.action.path, decision.track);
        (decision.action.kind == ActionKind::Delete ? deletes : rest).append(decision.action);
    };
    auto onDisk = [&disk](const QString& path) { return disk.value(path.toLower()); };  // not in the map = Missing

    QSet<QString> wanted;
    for (const File& file : next.files) {
        const QString key = file.path.toLower();
        wanted.insert(key);
        if (!isSafePath(file.path))
            add(decide(ActionKind::Skip, file.path, file.sha512, Why::BadPath, {}));
        else
            add(decideWanted(classify(file.path), file, old.value(key).sha512, onDisk(file.path)));
    }

    const PathSet released(next.released);
    for (auto it = old.cbegin(); it != old.cend(); ++it) {
        if (wanted.contains(it.key()))
            continue;
        const Old& entry = it.value();
        if (!isSafePath(entry.path)) {
            add(decide(ActionKind::Skip, entry.path, entry.sha512, Why::BadPath, {}));
            continue;
        }
        if (const auto decision = decideRemoved(classify(entry.path), entry.path, entry.sha512, onDisk(entry.path), released))
            add(*decision);
    }

    // Children before their folders, so a folder the pack replaces by a file is empty by the time the file is written.
    std::sort(deletes.begin(), deletes.end(), [](const Action& a, const Action& b) { return a.path.toLower() > b.path.toLower(); });
    std::sort(rest.begin(), rest.end(), [](const Action& a, const Action& b) { return a.path.toLower() < b.path.toLower(); });
    plan.actions = deletes + rest;
    return plan;
}

QStringList blobsNeeded(const Plan& plan)
{
    QSet<QString> blobs;
    for (const Action& action : plan.actions) {
        if (action.kind == ActionKind::Write || action.kind == ActionKind::PackNew)
            blobs.insert(action.sha512);
    }
    QStringList sorted(blobs.cbegin(), blobs.cend());
    sorted.sort();
    return sorted;
}

}  // namespace SharedPack
