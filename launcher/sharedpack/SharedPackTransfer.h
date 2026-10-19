// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <memory>

#include "SharedPackInvite.h"
#include "SharedPackManifest.h"

/// The pure rules of a download, kept apart from the Net classes (which need a running Application and cannot be unit tested): what a
/// request may go to, how much it may deliver, and what the finished bytes must be.
namespace SharedPack {

/// Checks one download while it streams. Reset it for every attempt (a redirect or a retry starts again).
class Limiter {
   public:
    struct Options {
        QStringList allowedHosts;  ///< "host" or "host:port", lower case; the only places this request may go
        qint64 maxBytes = 0;       ///< hard cap, enforced while streaming
        qint64 expectedSize = -1;  ///< exact size the manifest declared, -1 = unknown (latest.json, manifests)
        QString expectedSha512;    ///< lower-case hex, empty = no check
        bool requireHttps = true;  ///< only tests turn this off
    };

    explicit Limiter(Options options) : m_options(std::move(options)) {}

    /// Called before every request of the attempt, including a redirect target: https and an allowed host, no user info.
    bool checkUrl(const QUrl& url, QString* why);

    /// Starts a new attempt.
    void reset();

    /// Counts and hashes \a data. False (with a reason) as soon as the declared or the maximum size is exceeded; nothing more is accepted.
    bool add(const QByteArray& data, QString* why);

    /// Judges the finished response: HTTP 200, the exact size and the hash.
    bool finish(int httpStatus, QString* why);

    qint64 received() const { return m_received; }

   private:
    Options m_options;
    qint64 m_received = 0;
    bool m_failed = false;
    QCryptographicHash m_hash{ QCryptographicHash::Sha512 };
};

/// The sources to try for \a file, in order: its public URLs (Modrinth CDN) first, then the pack's own blob. Each carries the host it is
/// allowed to talk to.
struct Source {
    QUrl url;
    bool isBlob = false;
};
QList<Source> sourcesFor(const Invite& invite, const File& file);

using HeaderList = QList<QPair<QByteArray, QByteArray>>;

/// The SigV4 headers (x-amz-date, x-amz-content-sha256, Authorization) for a GET of \a url, or an empty list for a public invite or a URL
/// that is not on the invite's host: the keys are only ever sent to the host they belong to.
HeaderList signedGetHeaders(const Invite& invite, const QUrl& url, const QDateTime& now);

}  // namespace SharedPack
