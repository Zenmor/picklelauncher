// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>
#include <QStringList>

#include "SharedPackPlan.h"
#include "SharedPackState.h"

namespace SharedPack {

/// Where the files of an instance live.
struct Layout {
    QString gameDir;    ///< the game folder (minecraft/ or .minecraft/): every path of a pack is relative to it
    QString stateFile;  ///< <instance>/sharedpack-state.json
    QString
        workDir;  ///< <instance>/sharedpack-tmp: verified blobs in "blobs/<sha512>", the staging file in "stage"; on the instance's volume
    QString blobPath(const QString& sha512) const { return workDir + QStringLiteral("/blobs/") + sha512; }
};

struct ApplyResult {
    bool ok = false;
    QString error;  ///< for the user and the log, names the file; the state file is NOT written when this is set
    QStringList written;
    QStringList deleted;
    QStringList packNew;
    QStringList skipped;  ///< pack files left alone because the path is not safe (a link, a folder, a name Windows refuses): tell the user
    QStringList indexRemoved;  ///< metadata files of the launcher's own mod manager that described a jar we replaced or removed
};

/// The bytes that must be free before an update starts: the blobs to download and a second copy of each one that is written.
qint64 requiredFreeBytes(const Plan& plan, const Manifest& manifest);

/// Carries out \a plan, whose blobs are all verified in layout.workDir. Order: every action in plan order, the state file LAST, so a crash
/// leaves the old state and the same plan converges on the next run (files already in place plan as AlreadyCurrent).
///   - Write/PackNew: copy the blob next to nothing else (staging file), check its hash again, give it a NEW modification time (never the
///     source's: a replacement with the same size and time is invisible to the mod list), then rename over the target.
///   - Delete: only what the plan says; folders it leaves empty are removed.
///   - A target that is, or lies below, a link, reparse point or directory is refused (the scan should have said Skip already).
///   - Stale metadata in <folder>/.index for a jar we replace or remove is deleted: the pack is the truth about that file, and a left-over
///     entry would show a ghost mod.
/// The caller must have checked that the instance is not running.
ApplyResult applyPlan(const Layout& layout, const State& oldState, const Plan& plan);

/// The state after a successful apply of \a plan.
State stateAfter(const State& oldState, const Plan& plan);

}  // namespace SharedPack
