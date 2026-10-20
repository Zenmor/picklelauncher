// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <functional>
#include <memory>

#include "SharedPackOwner.h"
#include "SharedPackPublisher.h"
#include "SharedPackTarget.h"

namespace SharedPack {

/// PublishTarget that uses the Cloudflare Worker relay.
///
/// Phase 1 – prepare(): read-only /publish/check for a dry run, otherwise POST /publish/begin
///   • Gives the Worker: packId, revision, packSecret, blobs list.
///   • Receives one bounded list of missing blob hashes per request.
///   • Caches the result so run() can answer Head instantly and knows where
///     to PUT each blob.
///
/// Phase 2 – run() dispatches by key/kind:
///   • Head of a blob key: answered from the missing-blob map (present = not
///     in the map).
///   • Put::Blob: streamed through the Worker, which enforces the declared length and SHA-512.
///   • Put::Revision: PUT to the Worker, which validates it against the publish session.
///   • Put::Latest: calls POST /publish/commit; the Worker atomically writes
///     latest.json and updates D1.
///   • Get: fetched from readBaseUrl (for latest.json re-read before commit).
class RelayTarget : public PublishTarget {
    Q_OBJECT
   public:
    explicit RelayTarget(HostConfig host, QObject* parent = nullptr);
    ~RelayTarget() override;

    /// Must be called (by Publisher::publish) before prepare().
    void setJob(const PublishJob& job);

    using BlobRequests = QList<QPair<QString, qint64>>;  ///< (sha512, size)

    /// The blobs the Worker has to store: the files without a public URL (members fetch Modrinth files from the CDN).
    static BlobRequests blobRequests(const PublishJob& job);

    /// The JSON body of POST /publish/begin, including the blob list the Worker stores with the session.
    /// Static so a test can check it without a network.
    static QByteArray beginPayload(const HostConfig& host, const PublishJob& job);

    /// Calls /publish/begin and pages through bounded missing-blob batches. Always
    /// calls done() from the event loop.
    void prepare(bool dryRun, std::function<void(bool ok, const QString& error)> done) override;

    void rotateLink(const QString& packId, const QString& oldReadToken, const QString& packSecret,
                    std::function<void(bool ok, const QString& newReadToken, const QString& error)> done);
    void run(const Op& op, Done done, Progress progress = {}) override;
    void abortAll() override;
    void releaseSession() override;
    void reset() override;

   private:
    /// Call the Worker with JSON body \a payload (POST \a path) and call \a
    /// done(body, httpStatus) on the event loop.
    void workerPost(const QString& path, const QByteArray& payload,
                    std::function<void(const QByteArray& body, int status)> done);

    /// PUT \a data (or stream \a file) to \a presignedUrl; call \a done on the event loop.
    void workerPut(const QUrl& url, const QString& file, const QByteArray& body,
                      qint64 size, const Done& done, const Progress& progress);
    void checkBlobPage(int offset, std::function<void(bool ok, const QString& error)> done);
    void prepareBlobPage(int offset, std::function<void(bool ok, const QString& error)> done);

    /// POST /publish/commit.
    void doCommit(const Done& done);

    HostConfig m_host;
    std::optional<PublishJob> m_job;  ///< set via setJob() before prepare()

    // Results filled in by prepare():
    QMap<QString, QUrl> m_uploadUrls;  ///< sha512 → authenticated Worker upload URL
    BlobRequests m_blobRequests;
    QSet<QString> m_missingBlobs;
    QSet<QString> m_presentBlobs;
    QUrl m_revisionUploadUrl;           ///< Worker upload URL for revisions/<n>.json
    QString m_sessionId;
    bool m_dryRun = false;
    bool m_prepared = false;

    // Abort tracking
    bool m_aborted = false;
    QList<QObject*> m_active;  ///< QNetworkReply* tracked for abortAll()
};

}  // namespace SharedPack
