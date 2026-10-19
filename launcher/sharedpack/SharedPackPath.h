// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

namespace SharedPack {

/// How an update treats a file, decided only by where the file lives in the game folder.
enum class FileClass {
    PackWins,  ///< mods, resourcepacks, shaderpacks: the pack's file replaces the member's
    Merge,     ///< config and everything else: a file the member edited stays theirs
    SeedOnly,  ///< options.txt, servers.dat: written only when missing
    Excluded,  ///< saves, screenshots, logs, crash reports: never in a pack, never touched
};

/// Whether \a path is acceptable as a path relative to the game folder: forward slashes, no empty, "." or ".." segments, nothing absolute,
/// no drive letters or NTFS streams, no characters or names Windows refuses, no trailing dot or space, and not a name we generate.
/// Says why not in \a why when it is not.
bool isSafePath(const QString& path, QString* why = nullptr);

/// Case-insensitive, like the file systems the launcher runs on.
FileClass classify(const QString& path);

/// A set of files and folders (paths relative to the game folder, forward slashes). A folder covers everything below it. Case-insensitive.
class PathSet {
   public:
    PathSet() = default;
    explicit PathSet(const QStringList& paths);

    /// Whether \a path, or a folder above it, is in the set.
    bool covers(const QString& path) const;
    bool isEmpty() const { return m_lower.isEmpty(); }

   private:
    QSet<QString> m_lower;
};

}  // namespace SharedPack
