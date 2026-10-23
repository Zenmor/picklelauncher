// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>

#include "SharedPackManifest.h"

namespace SharedPack {

enum class Role {
    Member,  ///< follows a pack
    Owner,   ///< published it: the source instance is the truth and never receives updates
};

/// `<instance>/sharedpack-state.json`: what the installer wrote last time. It is the "old" side of the 3-way update.
struct State {
    int formatVersion = FormatVersion;
    QString packId;
    Role role = Role::Member;
    int revision = 0;                  ///< installed revision, 0 = nothing installed yet
    QMap<QString, QString> installed;  ///< path -> sha512 of the pack's version of the file as of the last update
    QString changelog;                 ///< the changelog of the installed revision (empty for old state files)
};

/// The file is local, but it can be edited or copied between machines, so it is validated like a manifest (paths included: they are used
/// to delete files).
Parsed<State> parseState(const QByteArray& json);
QByteArray stateToJson(const State& state);

enum class UpdateCheck {
    UpToDate,
    Available,
    OwnerNeverUpdates,
    WrongPack,   ///< the host answered with another pack
    HostBehind,  ///< the host answered with an older revision than the one installed: ignored, never move backwards
};

/// Decides from latest.json whether to offer an update. Only a strictly newer revision of the same pack, and never for the owner.
UpdateCheck checkForUpdate(const State& state, const Latest& latest);

}  // namespace SharedPack
