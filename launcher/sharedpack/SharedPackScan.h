// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>

#include "SharedPackPath.h"

namespace SharedPack {

struct ScannedFile {
    QString path;      ///< relative to the game folder, forward slashes
    QString absolute;  ///< where it is on this computer
    qint64 size = 0;
    QString sha512;    ///< lower-case hex: the identity of the content and the name of its blob
    QString sha256;    ///< lower-case hex: what an S3 PUT of the blob is signed with
};

struct ScanResult {
    QList<ScannedFile> files;  ///< only what would be published, sorted by path
    QStringList skipped;       ///< "path: why" for what was left out on purpose (links, folders that are never published)
    /// Files and folders that exist but were left out because the owner did not tick them (a folder is listed once, not its contents).
    /// Never hashed or entered. Becomes Manifest::released.
    QStringList unticked;
    QString error;             ///< a file could not be read, or the scan was cancelled: nothing may be published from this scan
    bool cancelled = false;

    explicit operator bool() const { return error.isEmpty(); }
};

/// Walks the game folder and hashes what a pack would carry: the file policy of isPublishablePath (saves, screenshots, logs, crash
/// reports, mod metadata and caches are not even entered), links and reparse points are skipped and reported (a link could point anywhere,
/// and the member side never follows one). Both hashes come from one read of each file. \a progress gets the bytes read so far; \a cancel
/// is polled between chunks. \a unticked is the owner's choice: what it covers is not entered and not hashed, on top of the policy above
/// (a ticked path that the policy refuses stays refused). Blocking: run it on a worker thread.
ScanResult scanGameFolder(const QString& gameDir, const std::atomic_bool* cancel = nullptr,
                          const std::function<void(qint64 bytesRead)>& progress = {}, const PathSet* unticked = nullptr);

/// The public Modrinth CDN address of each file, read from the answer of POST /version_files with algorithm sha512 (the same lookup
/// ModrinthPackExportTask does): for every requested hash the version it belongs to, and in it the file whose "hashes.sha512" IS that hash,
/// so the address is the one that serves exactly our bytes. Only addresses SharedPack::isAllowedUrl accepts are kept. The local mod
/// metadata ("url" in .index) is deliberately not used: nothing ties it to the file that is on disk now.
QHash<QString, QString> parseModrinthUrls(const QByteArray& response, const QStringList& sha512s);

}  // namespace SharedPack
