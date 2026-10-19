// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QFile>
#include <QString>
#include <functional>
#include <memory>

#include "QObjectPtr.h"
#include "SharedPackInvite.h"
#include "SharedPackTransfer.h"
#include "net/Download.h"
#include "net/HeaderProxy.h"
#include "net/Sink.h"
#include "tasks/Task.h"

/// The thin Net layer. Every rule (hosts, sizes, hashes, signing) lives in the pure SharedPackTransfer; this only plugs it into the
/// launcher's request classes: a Sink that applies the Limiter to every chunk, a header proxy that signs, and a Download that uses both.
namespace SharedPack {

/// Streams the response into memory or into a ".part" file that replaces the target only after the Limiter accepted the whole response.
/// Its init() runs again for every redirect, which is how a redirect to another host is refused before any header (the keys) is added.
class LimitedSink final : public Net::Sink {
   public:
    /// \a file empty = keep the bytes in memory. \a onViolation is called (once) when the Limiter refuses data: the request is aborted
    /// then, so nothing more is buffered.
    LimitedSink(Limiter::Options options, QString file, std::function<void()> onViolation);

    Task::State init(QNetworkRequest& request) override;
    Task::State write(QByteArray& data) override;
    Task::State abort() override;
    Task::State finalize(QNetworkReply& reply) override;
    bool hasLocalData() override { return false; }

    const QByteArray& bytes() const { return m_bytes; }
    bool violated() const { return m_violated; }

   private:
    Task::State fail(const QString& why);
    void discard();

    Limiter m_limiter;
    QString m_file;
    std::function<void()> m_onViolation;
    QFile m_part;
    QByteArray m_bytes;
    bool m_violated = false;
};

class SigV4HeaderProxy final : public Net::HeaderProxy {
   public:
    explicit SigV4HeaderProxy(Invite invite) : m_invite(std::move(invite)) {}
    QList<Net::HeaderPair> headers(const QNetworkRequest& request) const override;

   private:
    Invite m_invite;
};

class Request final : public Net::Download {
    Q_OBJECT
   public:
    Request() = default;

    /// A GET of \a url that follows the Limiter's rules. Signed when \a invite is an S3 invite and the URL is on its host.
    static shared_qobject_ptr<Request> make(const Invite& invite, const QUrl& url, Limiter::Options options, const QString& file = {});

    QByteArray bytes() const { return m_sink_ptr ? m_sink_ptr->bytes() : QByteArray(); }
    /// Why the Limiter refused the response, empty when it did not (a network failure has its own reason).
    QString violation() const { return m_sink_ptr && m_sink_ptr->violated() ? m_sink_ptr->failReason() : QString(); }

   private:
    LimitedSink* m_sink_ptr = nullptr;  ///< owned by the base class
};

/// Fetches one small document of the pack (latest.json, a revision manifest) into memory.
class FetchBytesTask final : public Task {
    Q_OBJECT
   public:
    FetchBytesTask(Invite invite, QString relativePath, qint64 maxBytes);

    QByteArray bytes() const { return m_bytes; }
    /// The HTTP status of the last answer, 0 when there was none. 401, 403 and 404 on latest.json mean the link is no longer valid.
    int httpStatus() const { return m_httpStatus; }

   protected:
    void executeTask() override;

   private:
    void tryFetch(int endpointIndex);

    Invite m_invite;
    QString m_relative;
    qint64 m_maxBytes;
    shared_qobject_ptr<Request> m_request;
    QByteArray m_bytes;
    int m_httpStatus = 0;
    int m_endpointIndex = 0;
};

}  // namespace SharedPack
