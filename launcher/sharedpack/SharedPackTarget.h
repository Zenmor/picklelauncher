// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QUrl>
#include <functional>

#include "SharedPackOwner.h"

namespace SharedPack {

/// One request to the host the owner publishes to. Keys are relative to the root of the host (the bucket, or the folder): they already
/// contain the prefix and the pack's token, see HostConfig::keyFor.
struct Op {
    enum class Kind {
        Head,    ///< is the key there?
        Get,     ///< read a small document (latest.json, a revision)
        Put,     ///< write \a body or the file \a file
        Delete,  ///< remove (a missing key is not an error, like on S3)
        Copy,    ///< \a copyFrom -> \a key, on the host itself (no data passes through this computer)
    };

    Kind kind = Kind::Head;
    QString key;
    QString copyFrom;
    QByteArray body;    ///< Put of a document
    QString file;       ///< Put of a blob: streamed from here
    qint64 size = 0;    ///< Put: the byte count of the body or the file
    QString sha256;     ///< Put of a file: payload hash for the signature (S3)
    QString sha512;     ///< Put of a file: its content hash; the folder target checks it while copying
    qint64 maxGet = 0;  ///< Get: the most bytes accepted
};

struct OpResult {
    bool ok = false;
    int status = 0;  ///< HTTP status, or the nearest equivalent for a folder (200, 404, 400, 500); 0 = no answer
    QString error;   ///< for the user and the log: never contains a key, a token or a URL
    QByteArray body; ///< Get

    /// Head and Get: the key is not there (as opposed to an error).
    bool absent() const { return !ok && status == 404; }
};

/// A key of a pack: "prefix/token/latest.json", "<token>/files/<sha512>": segments of A-Z a-z 0-9 . _ - only, no dot segments.
bool isValidKey(const QString& key);

/// Where a pack is published to. One implementation per kind of host; the publisher only talks to this.
class PublishTarget : public QObject {
    Q_OBJECT
   public:
    using Done = std::function<void(const OpResult&)>;
    using Progress = std::function<void(qint64 done, qint64 total)>;

    explicit PublishTarget(QObject* parent = nullptr) : QObject(parent) {}

    /// Called once before publish begins; result passed to \a done (false = publish is refused with \a error). Always from the event
    /// loop. \a dryRun may perform read-only checks but must not reserve or modify host state.
    virtual void prepare(bool dryRun, std::function<void(bool ok, const QString& error)> done)
    {
        Q_UNUSED(dryRun)
        done(true, {});
    }
    /// Starts \a op. \a done is called exactly once, always from the event loop (never from inside run()), also after abortAll().
    virtual void run(const Op& op, Done done, Progress progress = {}) = 0;
    /// Stops every operation in flight: their \a done is called with an "aborted" result.
    virtual void abortAll() {}
    /// Called when a publish ended without success and nobody cancelled it: gives back whatever the host reserved for it (a Shared Packs
    /// Server holds the pack until the session expires). Fire and forget; the target stays usable.
    virtual void releaseSession() {}
    /// Forgets what a previous, cancelled or failed publish left in the target, so it can start over.
    virtual void reset() {}
};

/// A folder on this computer. Write side only: the owner puts a web server, a sync client or a bucket mount in front of it; the members'
/// launcher never reads a folder (a pack invite is https only). Every write goes to a temporary file in the same folder and is renamed
/// into place, so a reader never sees half a file; a blob is checked against its sha512 while it is copied.
class FolderTarget : public PublishTarget {
    Q_OBJECT
   public:
    explicit FolderTarget(QString rootFolder, QObject* parent = nullptr) : PublishTarget(parent), m_root(std::move(rootFolder)) {}

    OpResult runSync(const Op& op) const;
    void run(const Op& op, Done done, Progress progress = {}) override;

   private:
    QString pathFor(const QString& key) const;
    QString m_root;
};

/// The wire form of one S3 request: what the Net layer sends. Pure, so it can be checked against the SigV4 vectors without a network.
struct S3Request {
    QString method;
    QUrl url;
    /// x-amz-date, x-amz-content-sha256, Authorization, and Content-Type / x-amz-copy-source when the operation has them.
    QList<QPair<QByteArray, QByteArray>> headers;
    bool valid = false;  ///< false: the operation cannot be sent (bad key, a file Put without its payload hash)
};

/// Path-style (https://<endpoint>/<bucket>/<key>), SigV4 through SharedPack::SigV4 with the host's WRITE keys. HTTPS only: the endpoint comes
/// from a validated HostConfig.
S3Request buildS3Request(const HostConfig& host, const Op& op, const QDateTime& now);

}  // namespace SharedPack
