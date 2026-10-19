// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <optional>

#include "SharedPackManifest.h"

namespace SharedPack {

/// Folders of a game folder that are caches of the game or of a mod loader: never published, on top of what classify() excludes.
/// (The member side does not need this: it only ever touches paths a manifest names.)
bool isCachePath(const QString& path);

/// Whether the owner's launcher publishes \a path: a safe path (see isSafePath), not Excluded by classify(), not a cache. \a why is set
/// when not.
bool isPublishablePath(const QString& path, QString* why = nullptr);

/// What the builder needs. \a files is everything found in the game folder (path relative to it, size, sha512) with the public URLs that
/// were checked to resolve; a file without URLs is one that needs its own blob on the host.
struct BuildInput {
    QString packId;
    QString name;
    int revision = 0;
    QString changelog;
    QList<Component> components;
    QList<File> files;
    /// The owner's unticked files and folders. A file under one never enters the manifest, whatever the scan delivered: the scan skips them
    /// too, this is the guarantee. It comes on top of isPublishablePath, never instead of it.
    QStringList unticked;
    /// Unticked files and folders that exist in the game folder (the scan's list): written to Manifest::released so members keep their copy.
    QStringList released;
};

struct BuildResult {
    std::optional<Manifest> manifest;
    /// "path: reason" for every file that was left out on purpose (saves, caches, paths a pack cannot carry). Not an error.
    QStringList skipped;
    /// Set without a manifest: what the owner has to change. English, for the log; the UI shows its own text around it.
    QString error;

    explicit operator bool() const { return manifest.has_value(); }
};

/// Applies the file policy of the planner (mods, resourcepacks and shaderpacks are the pack's; options.txt and servers.dat are seeds;
/// saves, screenshots, logs, crash reports and mod metadata never) to \a input and builds a manifest that parseManifest accepts: the
/// result is run through it before it is returned, so what comes out is exactly what a member would read.
/// Files are sorted by path. Too many files, a file or a total that is too big, two paths that differ only in case, a bad name or a bad
/// changelog are errors, not silent omissions: a pack that quietly lacks a file would break the game for every member.
BuildResult buildManifest(const BuildInput& input);

/// The documents as they are put on the host (compact JSON, UTF-8).
QByteArray manifestToJson(const Manifest& manifest);
QByteArray latestToJson(const Latest& latest);

/// Paths inside a pack folder.
QString latestKey();
QString revisionKey(int revision);
QString blobKey(const QString& sha512);

}  // namespace SharedPack
