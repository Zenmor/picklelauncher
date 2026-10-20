// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackRelayTarget.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

#include <algorithm>

#include "Application.h"
#include "SharedPackBuild.h"
#include "SharedPackInvite.h"
#include "SharedPackRedact.h"

namespace SharedPack {

namespace {

constexpr qint64 SmallResponseLimit = 2 * 1024 * 1024;

/// Extracts a human-readable error from a Worker JSON response.
QString describeWorkerStatus(int status, const QByteArray& body)
{
    const QJsonDocument doc = QJsonDocument::fromJson(body.left(4096));
    if (doc.isObject()) {
        const QString msg = doc.object().value(QStringLiteral("error")).toString();
        if (!msg.isEmpty())
            return msg;
    }
    if (status == 0)
        return QStringLiteral("no answer from the server");
    return QStringLiteral("the server returned HTTP %1").arg(status);
}

QJsonObject blobJson(const RelayTarget::BlobRequests::value_type& blob)
{
    return QJsonObject{ { QStringLiteral("sha512"), blob.first }, { QStringLiteral("size"), static_cast<double>(blob.second) } };
}

}  // namespace

RelayTarget::RelayTarget(HostConfig host, QObject* parent)
    : PublishTarget(parent), m_host(std::move(host))
{
    registerSecrets(m_host);
}

RelayTarget::~RelayTarget() = default;

void RelayTarget::setJob(const PublishJob& job)
{
    m_job = job;
    m_aborted = false;
    m_prepared = false;
    m_uploadUrls.clear();
    m_blobRequests.clear();
    m_missingBlobs.clear();
    m_presentBlobs.clear();
    m_revisionUploadUrl = {};
    m_sessionId.clear();
}

RelayTarget::BlobRequests RelayTarget::blobRequests(const PublishJob& job)
{
    BlobRequests requests;
    for (const auto& file : job.manifest.files) {
        if (file.sha512.isEmpty() || !file.urls.isEmpty())
            continue;
        requests.append({ file.sha512, file.size });
    }
    return requests;
}

QByteArray RelayTarget::beginPayload(const HostConfig& host, const PublishJob& job)
{
    QJsonObject body;
    body.insert(QStringLiteral("readToken"), job.token);
    if (!host.prefix.isEmpty())
        body.insert(QStringLiteral("prefix"), host.prefix);
    body.insert(QStringLiteral("packId"), job.manifest.packId);
    body.insert(QStringLiteral("revision"), job.manifest.revision);
    body.insert(QStringLiteral("packSecret"), host.packSecret);
    body.insert(QStringLiteral("manifestBytes"), static_cast<double>(manifestToJson(job.manifest).size()));
    // The Worker stores the blob list with the session and pages through it in /publish/prepare.
    QJsonArray blobs;
    for (const auto& blob : blobRequests(job))
        blobs.append(blobJson(blob));
    body.insert(QStringLiteral("blobs"), blobs);
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

// ---------------------------------------------------------------- prepare (phase 1)

void RelayTarget::prepare(bool dryRun, std::function<void(bool ok, const QString& error)> done)
{
    if (!m_job) {
        QTimer::singleShot(0, this, [done] { done(false, QStringLiteral("publish job not set on relay target")); });
        return;
    }

    // List only files that need an owned R2 blob; Modrinth URLs are fetched directly by members.
    m_blobRequests = blobRequests(*m_job);

    // The Worker refuses a file above this size with a vague "Invalid blob batch": name the files here, before anything is sent.
    constexpr qint64 MaxServerFileBytes = 100'000'000;
    QStringList tooBig;
    int tooBigCount = 0;
    for (const auto& file : m_job->manifest.files) {
        if (file.sha512.isEmpty() || !file.urls.isEmpty() || file.size <= MaxServerFileBytes)
            continue;
        if (++tooBigCount <= 3)
            tooBig.append(QStringLiteral("%1 (%2 MB)").arg(file.path).arg(file.size / 1'000'000));
    }
    if (tooBigCount > 0) {
        QString message = QStringLiteral("The server accepts files up to 100 MB. Too big: %1").arg(tooBig.join(QStringLiteral(", ")));
        if (tooBigCount > tooBig.size())
            message += QStringLiteral(" and %1 more").arg(tooBigCount - tooBig.size());
        QTimer::singleShot(0, this, [done, message] { done(false, message); });
        return;
    }

    if (dryRun) {
        m_dryRun = true;
        m_presentBlobs.clear();
        checkBlobPage(0, done);
        return;
    }

    m_dryRun = false;
    const QByteArray payload = beginPayload(m_host, *m_job);
    workerPost(QStringLiteral("/publish/begin"), payload, [this, done](const QByteArray& resp, int status) {
        if (m_aborted) {
            const QJsonDocument doc = QJsonDocument::fromJson(resp);
            if (status >= 200 && status <= 299 && doc.isObject()) {
                m_sessionId = doc.object().value(QStringLiteral("sessionId")).toString();
                if (!m_sessionId.isEmpty()) {
                    const QByteArray abortPayload =
                        QJsonDocument(QJsonObject{ { QStringLiteral("sessionId"), m_sessionId } }).toJson(QJsonDocument::Compact);
                    workerPost(QStringLiteral("/publish/abort"), abortPayload, [](const QByteArray&, int) {});
                }
            }
            done(false, QStringLiteral("aborted"));
            return;
        }
        if (status < 200 || status > 299) {
            done(false, describeWorkerStatus(status, resp));
            return;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(resp);
        if (!doc.isObject()) {
            done(false, QStringLiteral("the server returned an unexpected response"));
            return;
        }
        const QJsonObject obj = doc.object();
        m_sessionId = obj.value(QStringLiteral("sessionId")).toString();
        if (m_sessionId.isEmpty()) {
            done(false, QStringLiteral("the server did not return a publish session"));
            return;
        }
        QUrl revisionUrl(m_host.endpoint, QUrl::StrictMode);
        QString path = revisionUrl.path();
        while (path.endsWith(QLatin1Char('/')))
            path.chop(1);
        revisionUrl.setPath(path + QStringLiteral("/publish/revision/") + m_sessionId);
        m_revisionUploadUrl = revisionUrl;
        m_missingBlobs.clear();
        m_uploadUrls.clear();
        prepareBlobPage(0, done);
    });
}

void RelayTarget::checkBlobPage(int offset, std::function<void(bool ok, const QString& error)> done)
{
    if (m_aborted) {
        done(false, QStringLiteral("aborted"));
        return;
    }
    if (offset >= m_blobRequests.size()) {
        m_prepared = true;
        done(true, {});
        return;
    }
    QJsonArray blobs;
    const int end = static_cast<int>(std::min<qsizetype>(offset + 100, m_blobRequests.size()));
    for (int i = offset; i < end; ++i)
        blobs.append(blobJson(m_blobRequests.at(i)));
    const QByteArray payload = QJsonDocument(QJsonObject{
        { QStringLiteral("readToken"), m_job->token },
        { QStringLiteral("prefix"), m_host.prefix },
        { QStringLiteral("blobs"), blobs },
    }).toJson(QJsonDocument::Compact);
    workerPost(QStringLiteral("/publish/check"), payload, [this, offset, end, done](const QByteArray& response, int status) {
        if (m_aborted) return done(false, QStringLiteral("aborted"));
        if (status < 200 || status > 299) return done(false, describeWorkerStatus(status, response));
        const QJsonDocument document = QJsonDocument::fromJson(response);
        if (!document.isObject() || !document.object().value(QStringLiteral("presentBlobs")).isArray()) {
            return done(false, QStringLiteral("the server returned an unexpected response"));
        }
        for (const QJsonValue& value : document.object().value(QStringLiteral("presentBlobs")).toArray()) {
            if (!value.isString()) return done(false, QStringLiteral("the server returned an invalid blob list"));
            m_presentBlobs.insert(value.toString());
        }
        checkBlobPage(end, done);
    });
}

void RelayTarget::prepareBlobPage(int offset, std::function<void(bool ok, const QString& error)> done)
{
    if (m_aborted) {
        done(false, QStringLiteral("aborted"));
        return;
    }
    QJsonObject body{
        { QStringLiteral("sessionId"), m_sessionId },
        { QStringLiteral("offset"), offset },
    };
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    workerPost(QStringLiteral("/publish/prepare"), payload, [this, offset, done](const QByteArray& response, int status) {
        if (m_aborted) return done(false, QStringLiteral("aborted"));
        if (status < 200 || status > 299) return done(false, describeWorkerStatus(status, response));
        const QJsonDocument document = QJsonDocument::fromJson(response);
        if (!document.isObject()) return done(false, QStringLiteral("the server returned an unexpected response"));
        const QJsonObject object = document.object();
        const QJsonArray missing = object.value(QStringLiteral("missingBlobs")).toArray();
        const int nextOffset = object.value(QStringLiteral("nextOffset")).toInt(-1);
        const bool complete = object.value(QStringLiteral("complete")).toBool(false);
        const int expectedNext = static_cast<int>(std::min<qsizetype>(offset + 100, m_blobRequests.size()));
        if (nextOffset != expectedNext || complete != (nextOffset == m_blobRequests.size())) {
            return done(false, QStringLiteral("the server returned an invalid upload batch"));
        }
        for (const QJsonValue& value : missing) {
            if (!value.isString()) return done(false, QStringLiteral("the server returned an invalid blob list"));
            const QString sha512 = value.toString();
            const bool requested = std::any_of(m_blobRequests.cbegin() + offset, m_blobRequests.cbegin() + nextOffset,
                                               [&sha512](const auto& item) { return item.first == sha512; });
            if (!requested) return done(false, QStringLiteral("the server returned an unexpected blob"));
            QUrl uploadUrl(m_host.endpoint, QUrl::StrictMode);
            QString path = uploadUrl.path();
            while (path.endsWith(QLatin1Char('/')))
                path.chop(1);
            path += QStringLiteral("/publish/upload/") + m_sessionId + QLatin1Char('/') + sha512;
            uploadUrl.setPath(path);
            m_missingBlobs.insert(sha512);
            m_uploadUrls.insert(sha512, uploadUrl);
        }
        if (complete) {
            QUrl revisionUrl(m_host.endpoint, QUrl::StrictMode);
            QString path = revisionUrl.path();
            while (path.endsWith(QLatin1Char('/')))
                path.chop(1);
            revisionUrl.setPath(path + QStringLiteral("/publish/revision/") + m_sessionId);
            m_revisionUploadUrl = revisionUrl;
            m_prepared = true;
            done(true, {});
        } else {
            prepareBlobPage(nextOffset, done);
        }
    });
}

void RelayTarget::rotateLink(const QString& packId, const QString& oldReadToken, const QString& packSecret,
                             std::function<void(bool ok, const QString& newReadToken, const QString& error)> done)
{
    const QByteArray payload = QJsonDocument(QJsonObject{
        { QStringLiteral("packId"), packId },
        { QStringLiteral("oldReadToken"), oldReadToken },
        { QStringLiteral("packSecret"), packSecret },
    }).toJson(QJsonDocument::Compact);
    workerPost(QStringLiteral("/publish/rotate"), payload, [done](const QByteArray& response, int status) {
        if (status < 200 || status > 299) return done(false, {}, describeWorkerStatus(status, response));
        const QJsonDocument document = QJsonDocument::fromJson(response);
        const QString token = document.isObject() ? document.object().value(QStringLiteral("readToken")).toString() : QString();
        if (!isValidToken(token)) return done(false, {}, QStringLiteral("the server returned an invalid rotated invite"));
        done(true, token, {});
    });
}

// ---------------------------------------------------------------- run

void RelayTarget::run(const Op& op, Done done, Progress progress)
{
    if (m_aborted) {
        QTimer::singleShot(0, this, [done] {
            OpResult r;
            r.error = QStringLiteral("aborted");
            done(r);
        });
        return;
    }

    switch (op.kind) {
        case Op::Kind::Head: {
            // The Worker told us which blobs are missing (those have a presigned URL).
            // Answer instantly from the map: no network round-trip.
            const QString sha512 = op.key.section(QLatin1Char('/'), -1);
            const bool missing = m_dryRun ? !m_presentBlobs.contains(sha512) : m_missingBlobs.contains(sha512);
            QTimer::singleShot(0, this, [done, missing] {
                OpResult r;
                r.ok = !missing;           // ok = present on host
                r.status = missing ? 404 : 200;
                done(r);
            });
            return;
        }

        case Op::Kind::Get: {
            QString relative = op.key;
            if (!m_host.prefix.isEmpty()) {
                const QString prefix = m_host.prefix + QLatin1Char('/');
                if (!relative.startsWith(prefix)) {
                    QTimer::singleShot(0, this, [done] {
                        OpResult r;
                        r.error = QStringLiteral("the relay read key was invalid");
                        done(r);
                    });
                    return;
                }
                relative.remove(0, prefix.size());
            }
            const QString tokenPrefix = (m_job ? m_job->token : op.key.section(QLatin1Char('/'), -2, -2)) + QLatin1Char('/');
            if (!relative.startsWith(tokenPrefix)) {
                QTimer::singleShot(0, this, [done] {
                    OpResult r;
                    r.error = QStringLiteral("the relay read key was invalid");
                    done(r);
                });
                return;
            }
            relative.remove(0, tokenPrefix.size());
            Invite invite;
            invite.version = 2;
            invite.endpoint = m_host.endpoint + QStringLiteral("/read");
            invite.token = m_job ? m_job->token : tokenPrefix.chopped(1);
            const QUrl url = invite.urlFor(relative);
            if (!url.isValid()) {
                QTimer::singleShot(0, this, [done] {
                    OpResult r;
                    r.error = QStringLiteral("could not build public read URL");
                    done(r);
                });
                return;
            }
            QNetworkRequest request(url);
            QNetworkAccessManager* net = APPLICATION->network();
            QNetworkReply* reply = net->get(request);
            m_active.append(reply);
            const qint64 maxGet = op.maxGet > 0 ? op.maxGet : 8 * 1024 * 1024;
            connect(reply, &QNetworkReply::finished, this, [this, reply, done, maxGet] {
                m_active.removeOne(reply);
                const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                OpResult r;
                r.status = status;
                if (status == 404) {
                    r.ok = false;  // absent — ok for missing latest.json on first publish
                } else if (status >= 200 && status <= 299) {
                    r.ok = true;
                    r.body = reply->read(maxGet);
                } else {
                    r.error = status > 0 ? QStringLiteral("HTTP %1").arg(status)
                                         : QStringLiteral("no answer from the server");
                }
                reply->deleteLater();
                done(r);
            });
            return;
        }

        case Op::Kind::Put: {
            if (!op.file.isEmpty()) {
                // Blob: stream through the Worker for exact length and SHA-512 verification.
                const QUrl presignedUrl = m_uploadUrls.value(op.sha512);
                if (!presignedUrl.isValid()) {
                    // Blob already exists on the host — should not happen (Head said missing),
                    // but handle gracefully: report success so the sequencer moves on.
                    QTimer::singleShot(0, this, [done] {
                        OpResult r;
                        r.ok = true;
                        r.status = 200;
                        done(r);
                    });
                    return;
                }
                workerPut(presignedUrl, op.file, {}, op.size, done, progress);
                return;
            }

            // Document: revision.json or latest.json, distinguished by the last key segment.
            const QString lastSegment = op.key.section(QLatin1Char('/'), -1);
            if (lastSegment == QStringLiteral("latest.json")) {
                // Commit: tell the Worker; it atomically writes latest.json.
                doCommit(done);
                return;
            }

            // revisions/N.json: validated Worker upload.
            if (m_revisionUploadUrl.isValid()) {
                workerPut(m_revisionUploadUrl, {}, op.body, op.size, done, progress);
                return;
            }

            QTimer::singleShot(0, this, [done] {
                OpResult r;
                r.error = QStringLiteral("revision upload URL not set");
                done(r);
            });
            return;
        }

        case Op::Kind::Delete:
        case Op::Kind::Copy: {
            // Not used for Relay packs.
            QTimer::singleShot(0, this, [done] {
                OpResult r;
                r.error = QStringLiteral("operation not supported on a Shared Packs Server");
                done(r);
            });
            return;
        }
    }
}

void RelayTarget::abortAll()
{
    m_aborted = true;
    const auto active = m_active;
    for (QObject* obj : active) {
        if (auto* reply = qobject_cast<QNetworkReply*>(obj))
            reply->abort();
    }
    releaseSession();
}

void RelayTarget::releaseSession()
{
    // The Worker only aborts a session that is still 'active': after a commit it answers 409, which is harmless here.
    if (m_sessionId.isEmpty())
        return;
    const QByteArray payload = QJsonDocument(QJsonObject{ { QStringLiteral("sessionId"), m_sessionId } }).toJson(QJsonDocument::Compact);
    m_sessionId.clear();
    workerPost(QStringLiteral("/publish/abort"), payload, [](const QByteArray&, int) {});
}

void RelayTarget::reset()
{
    m_aborted = false;
    m_sessionId.clear();
}

// ---------------------------------------------------------------- helpers

void RelayTarget::workerPost(const QString& path, const QByteArray& payload,
                              std::function<void(const QByteArray& body, int status)> done)
{
    QString base = m_host.endpoint;
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    QNetworkRequest request(QUrl(base + path, QUrl::StrictMode));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArray("application/json; charset=utf-8"));
    request.setRawHeader("Authorization",
                         QByteArray("Bearer ") + m_host.publisherToken.toUtf8());

    QNetworkAccessManager* net = APPLICATION->network();
    QNetworkReply* reply = net->post(request, payload);
    m_active.append(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply, done] {
        m_active.removeOne(reply);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll().left(SmallResponseLimit);
        reply->deleteLater();
        done(body, status);
    });
}

void RelayTarget::workerPut(const QUrl& url, const QString& file, const QByteArray& body,
                                qint64 size, const Done& done, const Progress& progress)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentLengthHeader, size);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/octet-stream"));
    request.setRawHeader("Authorization", QByteArray("Bearer ") + m_host.publisherToken.toUtf8());

    QNetworkAccessManager* net = APPLICATION->network();
    QNetworkReply* reply = nullptr;

    if (!file.isEmpty()) {
        auto* f = new QFile(file, this);
        if (!f->open(QIODevice::ReadOnly) || f->size() != size) {
            f->deleteLater();
            QTimer::singleShot(0, this, [done] {
                OpResult r;
                r.error = QStringLiteral("the file to upload cannot be read");
                done(r);
            });
            return;
        }
        f->seek(0);
        reply = net->put(request, f);
        // QNetworkAccessManager does NOT own the device; delete it after the reply finishes.
        connect(reply, &QNetworkReply::finished, f, &QObject::deleteLater);
    } else {
        reply = net->put(request, body);
    }

    m_active.append(reply);

    if (progress) {
        connect(reply, &QNetworkReply::uploadProgress, this,
                [progress](qint64 sent, qint64) { progress(sent, 0); });
    }

    connect(reply, &QNetworkReply::finished, this, [this, reply, done] {
        m_active.removeOne(reply);
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();
        OpResult r;
        r.status = status;
        if (status >= 200 && status <= 299) {
            r.ok = true;
        } else {
            r.error = status > 0 ? QStringLiteral("the server refused the upload (HTTP %1)").arg(status)
                                 : QStringLiteral("no answer from the server");
        }
        done(r);
    });
}

void RelayTarget::doCommit(const Done& done)
{
    QJsonObject body;
    body.insert(QStringLiteral("packId"), m_job->manifest.packId);
    body.insert(QStringLiteral("revision"), m_job->manifest.revision);
    body.insert(QStringLiteral("packSecret"), m_host.packSecret);
    body.insert(QStringLiteral("sessionId"), m_sessionId);

    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    workerPost(QStringLiteral("/publish/commit"), payload, [done](const QByteArray& resp, int status) {
        OpResult r;
        r.status = status;
        if (status >= 200 && status <= 299) {
            r.ok = true;
        } else {
            r.error = describeWorkerStatus(status, resp);
        }
        done(r);
    });
}

}  // namespace SharedPack
