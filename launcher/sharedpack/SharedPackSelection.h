// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>
#include <QStringList>
#include <optional>

namespace SharedPack {

/// The Shared Packs Server refuses a single file above this many bytes ("Invalid blob batch"). Only a file that needs its own uploaded blob
/// counts: one that members fetch from Modrinth's CDN is not uploaded at all.
constexpr qint64 ServerFileLimit = 100'000'000;

/// The top-level folders of the game folder that a NEW pack ticks: the things that make the pack. Everything else (mod caches, a mod's own
/// data, downloaded runtimes, loose files) starts unticked and is the owner's choice.
QStringList defaultTickedFolders();

/// Of \a topLevelNames (the names directly inside the game folder, files and folders) the ones that are NOT ticked by default.
/// Case-insensitive, like the file systems the launcher runs on. Pure.
QStringList defaultUnticked(const QStringList& topLevelNames);

/// What the owner decided for one pack, as it is saved.
struct StoredSelection {
    QStringList unticked;  ///< files and folders (relative to the game folder) the owner does not publish
    QStringList known;     ///< the top-level names that existed the last time the owner saw the tree
};

/// The unticked paths the Publish dialog starts from.
/// - Saved choices: they stay as they are, and a top-level name that did not exist when they were saved (a mod made a new folder) follows
///   the defaults instead of being published unasked.
/// - Nothing saved, a new pack: the defaults.
/// - Nothing saved, \a keepEverythingTicked: for a pack that was already published before this choice existed, so nothing it carries
///   silently drops out of it.
QStringList initialUnticked(const std::optional<StoredSelection>& stored, const QStringList& topLevelNames, bool keepEverythingTicked);

/// <instance>/sharedpack-selection.json, next to sharedpack-link.json and sharedpack-state.json.
QString selectionPath(const QString& instanceRoot);

/// Empty when there is no file or it is damaged (the defaults apply then, which publishes less, not more). Entries that are not safe paths
/// are dropped.
std::optional<StoredSelection> readSelection(const QString& instanceRoot);

/// Writes it atomically. False when the file could not be saved.
bool writeSelection(const QString& instanceRoot, const StoredSelection& selection);

}  // namespace SharedPack
