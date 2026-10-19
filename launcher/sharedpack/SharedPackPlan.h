// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QHash>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include "SharedPackManifest.h"
#include "SharedPackState.h"

namespace SharedPack {

enum class DiskKind {
    Missing,
    File,    ///< a regular file, \a sha512 is its hash
    Unsafe,  ///< a directory, symlink, reparse point or unreadable file where the pack has a file: never touched
};

struct DiskEntry {
    DiskKind kind = DiskKind::Missing;
    QString sha512;
};

/// What is on disk, keyed by LOWER-CASE path (the file systems we run on ignore case). A path that is not in the map is missing.
using DiskState = QHash<QString, DiskEntry>;

enum class ActionKind {
    Write,    ///< put the pack's file in place (the blob named by Action::sha512)
    Delete,   ///< remove the file; the caller also removes folders this leaves empty
    Keep,     ///< leave the file alone
    PackNew,  ///< leave the file alone and write the pack's version next to it as "<path>.packnew"
    Skip,     ///< refuse to touch this path
};

enum class Why {
    NewFile,           ///< Write: not there yet
    PackChanged,       ///< Write: the pack's file differs from what is there, and the member did not edit it (or the pack wins)
    SeedMissing,       ///< Write: a seed file that is not there
    RestoredChanged,   ///< Write: the member deleted a file the pack has changed since
    AlreadyCurrent,    ///< Keep: the file already is the pack's version
    UserEdited,        ///< Keep: the member edited it and the pack did not change it
    UserDeleted,       ///< Keep: the member deleted it and the pack did not change it
    SeedExists,        ///< Keep: seed files are never overwritten or deleted
    RemovedButEdited,  ///< Keep: the pack dropped the file but the member changed it
    Released,          ///< Keep: the owner stopped publishing the file on purpose (Manifest::released): it stays and is no longer tracked
    PackRemoved,       ///< Delete: the pack dropped the file and it is still the pack's
    Conflict,          ///< PackNew: the member and the pack both changed it, or the member already had a file of that name
    Excluded,          ///< Skip: saves, screenshots, logs, crash reports, launcher metadata
    UnsafeOnDisk,      ///< Skip: not a regular file
    BadPath,           ///< Skip: failed SharedPack::isSafePath (a validated manifest never gets here)
};

struct Action {
    ActionKind kind = ActionKind::Keep;
    QString path;    ///< as the manifest (or, for a removal, the state file) spells it
    QString sha512;  ///< the blob to write for Write and PackNew; the old hash for Delete and Keep after a removal
    Why why = Why::AlreadyCurrent;
};

struct Plan {
    enum class Refusal {
        None,
        OwnerRole,  ///< the owner's source instance never receives updates
        WrongPack,
        Backwards,  ///< the manifest is older than what is installed
    };

    Refusal refusal = Refusal::None;
    int revision = 0;                  ///< the revision this plan installs
    QList<Action> actions;             ///< deletes first (deepest first), then everything else by path
    QMap<QString, QString> installed;  ///< State::installed once every action succeeded

    explicit operator bool() const { return refusal == Refusal::None; }
};

/// The 3-way update: \a state is what the installer wrote last time ("old"), \a next the new manifest, \a disk what is there now.
/// Pure: decides, touches nothing. Files that are in neither the state nor the manifest are the member's and never appear in the plan.
Plan planUpdate(const State& state, const Manifest& next, const DiskState& disk);

/// The blobs (sha512) the plan needs, each once, sorted: the only things to download.
QStringList blobsNeeded(const Plan& plan);

}  // namespace SharedPack
