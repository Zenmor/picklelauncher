// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>
#include <optional>

#include "SharedPackApply.h"
#include "SharedPackInvite.h"

namespace SharedPack {

/// What an instance remembers about the pack it follows: the invite (it holds the token and read keys, so it lives in its own file in
/// the instance folder, never in instance.cfg) and the member's auto-update choice.
struct Link {
    Invite invite;
    bool autoUpdate = false;
};

/// The files of an instance: <instance>/sharedpack-link.json, sharedpack-state.json and the work folder sharedpack-tmp.
Layout layoutFor(const QString& instanceRoot, const QString& gameDir);
QString linkPath(const QString& instanceRoot);

/// Reads the link of an instance; empty when there is none or it is damaged (the invite is validated again).
std::optional<Link> readLink(const QString& instanceRoot);
/// Writes it atomically. False when the file could not be saved.
bool writeLink(const QString& instanceRoot, const Link& link);

/// The state of an instance, a fresh member state for \a packId when there is no file yet (first install). Empty when the file exists but
/// is damaged or belongs to another pack: nothing is guessed in that case.
std::optional<State> readStateOrFresh(const Layout& layout, const QString& packId);

/// Writes the state file atomically (temporary file, rename). False when it could not be saved.
bool writeState(const Layout& layout, const State& state);

}  // namespace SharedPack
