// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <optional>

#include "SharedPackManifest.h"

namespace SharedPack {

// ------------------------------------------------------------------ revision numbers

/// The revision to publish next. Never reuses a number and never goes backwards: the larger of what the host announces and what this
/// instance last published or installed, plus one. \a hostLatest is empty when the host has no latest.json (first publish, or the folder
/// was removed). 0 when there is no number left (Limits::MaxRevision).
/// An owner "rollback" is not a lower number: it is the old content published as a NEW revision, see rollbackManifest.
int nextRevision(std::optional<int> hostLatest, int localRevision);

/// \a old (a manifest read back from the host) as the new revision \a revision: same files and components, a new changelog.
Manifest rollbackManifest(const Manifest& old, int revision, const QString& changelog);

/// Two owners publishing at once cannot be made safe on a store without conditional writes: the last PUT of latest.json wins and the other
/// owner's revision is orphaned. The best the publisher can do is read latest.json again right before it writes it and stop when it moved.
/// \a baseline is what it read when the publish started, \a now what it reads just before the final PUT (both empty = no latest.json).
bool latestUnchanged(std::optional<int> baseline, std::optional<int> now);

// ------------------------------------------------------------------ upload plan

/// A file the owner has on disk that can serve as the blob named by its sha512.
struct LocalBlob {
    QString path;
    qint64 size = 0;
    QString sha256;  ///< lower-case hex SHA-256 of the file: the payload hash an S3 PUT is signed with (computed together with the sha512)
};

struct Put {
    enum class Kind {
        Blob,      ///< files/<sha512>, streamed from \a file
        Revision,  ///< revisions/<n>.json, \a body
        Latest,    ///< latest.json, \a body: always last
    };

    Kind kind = Kind::Blob;
    QString key;       ///< relative to the pack folder
    QString file;      ///< Blob: the local file
    QByteArray body;   ///< Revision and Latest: the document
    qint64 size = 0;   ///< bytes of the body or of the file
    QString sha512;    ///< Blob: the content hash it must have
    QString sha256;    ///< Blob: the payload hash for signing
};

struct UploadPlan {
    QList<Put> puts;  ///< blobs (largest first), then the revision, then latest.json
    QString error;    ///< set instead of puts: what cannot be done (a needed blob that is neither on the host nor on disk)
    qint64 blobBytes = 0;
    int blobCount = 0;

    explicit operator bool() const { return error.isEmpty(); }
};

/// A file that has no public URL needs its own blob on the host.
bool needsBlob(const File& file);

/// What must be put on the host to publish \a manifest: a blob for every file that needs one and is not on the host yet (\a hostBlobs is
/// the set of sha512 that a HEAD found there; everything else is assumed missing), then revisions/<n>.json, then latest.json LAST. Two
/// files with the same content share one blob. Pure: no clock, no network.
UploadPlan planUpload(const Manifest& manifest, const QSet<QString>& hostBlobs, const QHash<QString, LocalBlob>& localBlobs);

/// The hashes planUpload needs a HEAD answer for: every blob the manifest needs, once each, sorted.
QStringList blobsToCheck(const Manifest& manifest);

/// Runs an UploadPlan with bounded concurrency without ever breaking its order: blobs in any order up to \a maxConcurrent at a time, the
/// revision only when every blob is in, latest.json only when the revision is in. After one failure nothing more is started, so a failed
/// upload can never leave latest.json pointing at a revision that is not fully there. Pure bookkeeping: the caller does the PUTs.
class UploadSequencer {
   public:
    UploadSequencer(UploadPlan plan, int maxConcurrent);

    /// The indices (into plan().puts) that may start now. Call again after every done().
    QList<int> next();
    /// Reports the end of a started put.
    void done(int index, bool ok);

    bool failed() const { return m_failed; }
    /// Nothing in flight and either everything is in or a failure stopped the run.
    bool finished() const { return m_inFlight == 0 && (m_failed || m_completed == m_plan.puts.size()); }
    bool succeeded() const { return !m_failed && m_completed == m_plan.puts.size(); }
    int completed() const { return m_completed; }
    const UploadPlan& plan() const { return m_plan; }

   private:
    UploadPlan m_plan;
    int m_max;
    int m_started = 0;  ///< puts are started strictly in plan order
    int m_inFlight = 0;
    int m_completed = 0;
    bool m_failed = false;
};

// ------------------------------------------------------------------ rotate

/// Moving a pack to a new token. Members only ever read the latest revision, so that is what moves: its blobs, its manifest and latest.json.
struct RotatePlan {
    QStringList copyKeys;    ///< blobs, then revisions/<n>.json, then latest.json LAST (relative to the folder)
    QStringList deleteKeys;  ///< latest.json FIRST (the old link stops working at once), then every revisions/1..n.json, then the blobs
};

/// \a manifest is the latest revision. Blobs that are not needed (the file has a public URL) are not copied; older revisions' own blobs
/// that the latest no longer refers to are NOT found here (no LIST), so they stay behind in the old folder: said honestly in the UI.
RotatePlan planRotate(const Manifest& manifest);

}  // namespace SharedPack
