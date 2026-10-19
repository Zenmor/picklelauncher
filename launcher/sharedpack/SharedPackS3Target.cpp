// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackS3Target.h"

#include <QDateTime>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <memory>

#include "Application.h"
#include "SharedPackRedact.h"
#include "SharedPackTransfer.h"
#include "net/Sink.h"

namespace SharedPack {

namespace {

constexpr qint64 SmallBodyLimit = 64 * 1024;

/// Keeps the (small) answer in memory and accepts any 2xx; the host check of the Limiter runs for the first request and for every
/// redirect, so the write keys never go anywhere but the configured endpoint.
class AnswerSink final : public Net::Sink {
   public:
    AnswerSink(const QString& host, qint64 maxBytes) : m_limiter(options(host, maxBytes)) {}

    Task::State init(QNetworkRequest& request) override
    {
        m_limiter.reset();
        m_bytes.clear();
        QString why;
        if (!m_limiter.checkUrl(request.url(), &why)) {
            m_fail_reason = why;
            return Task::State::Failed;
        }
        return Task::State::Running;
    }
    Task::State write(QByteArray& data) override
    {
        QString why;
        if (!m_limiter.add(data, &why)) {
            m_fail_reason = why;
            return Task::State::Failed;
        }
        m_bytes.append(data);
        return Task::State::Running;
    }
    Task::State abort() override { return Task::State::Failed; }
    Task::State finalize(QNetworkReply& reply) override
    {
        const int status = reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status < 200 || status > 299) {
            m_fail_reason = QStringLiteral("HTTP %1").arg(status);
            return Task::State::Failed;
        }
        return Task::State::Succeeded;
    }
    bool hasLocalData() override { return false; }

    const QByteArray& bytes() const { return m_bytes; }

   private:
    static Limiter::Options options(const QString& host, qint64 maxBytes)
    {
        Limiter::Options result;
        result.allowedHosts = { host.toLower() };
        result.maxBytes = maxBytes;
        return result;
    }

    Limiter m_limiter;
    QByteArray m_bytes;
};

QString describeStatus(int status, const QByteArray& body)
{
    QString text;
    switch (status) {
        case 400: text = QStringLiteral("the host did not accept the request"); break;
        case 401:
        case 403: text = QStringLiteral("the host refused the keys or the request"); break;
        case 404: text = QStringLiteral("not found"); break;
        case 409:
        case 412: text = QStringLiteral("the host reported a conflict"); break;
        case 429: text = QStringLiteral("the host asks to slow down"); break;
        case 0: text = QStringLiteral("no answer from the host"); break;
        default: text = status >= 500 ? QStringLiteral("the host had a problem") : QStringLiteral("the host did not accept the request"); break;
    }
    if (status > 0)
        text += QStringLiteral(" (HTTP %1)").arg(status);
    // S3 names the problem in the error document ("SignatureDoesNotMatch", "NoSuchBucket"): a short code, never a key.
    const auto match = QRegularExpression(QStringLiteral("<Code>([A-Za-z0-9]{1,64})</Code>")).match(QString::fromUtf8(body.left(2048)));
    if (match.hasMatch())
        text += QStringLiteral(": ") + match.captured(1);
    return text;
}

}  // namespace

class S3Operation final : public Net::NetRequest {
   public:
    S3Operation(HostConfig host, Op op) : m_host(std::move(host)), m_op(std::move(op))
    {
        setObjectName(QStringLiteral("SHAREDPACK-S3"));
        setNetwork(APPLICATION->network());
        const qint64 limit = m_op.kind == Op::Kind::Get ? (m_op.maxGet > 0 ? m_op.maxGet : 8 * 1024 * 1024) : SmallBodyLimit;
        auto sink = std::make_unique<AnswerSink>(QUrl(m_host.endpoint).authority(), limit);
        m_answer = sink.get();
        m_sink = std::move(sink);
        setUrl(buildS3Request(m_host, m_op, QDateTime::currentDateTimeUtc()).url);
    }

    QByteArray answer() const { return m_answer->bytes(); }
    bool wasAborted() const { return m_aborted; }
    bool abort() override
    {
        m_aborted = true;
        return Net::NetRequest::abort();
    }

   private:
    QNetworkReply* getReply(QNetworkRequest& request) override
    {
        // Signed here, not in the constructor: a redirect or a retry runs this again, and a signature is only valid for minutes.
        const S3Request signedRequest = buildS3Request(m_host, m_op, QDateTime::currentDateTimeUtc());
        if (!signedRequest.valid || signedRequest.url != request.url()) {
            emitFailed(QStringLiteral("the request could not be built"));
            return nullptr;
        }
        for (const auto& header : signedRequest.headers)
            request.setRawHeader(header.first, header.second);

        switch (m_op.kind) {
            case Op::Kind::Head:
                return m_network->head(request);
            case Op::Kind::Get:
                return m_network->get(request);
            case Op::Kind::Delete:
                return m_network->deleteResource(request);
            case Op::Kind::Copy:
                return m_network->put(request, QByteArray());
            case Op::Kind::Put:
                if (m_op.file.isEmpty())
                    return m_network->put(request, m_op.body);
                // Streamed from disk: nothing is held in memory. Opened (or rewound) for every attempt.
                if (!m_file)
                    m_file = std::make_unique<QFile>(m_op.file);
                if (!m_file->isOpen() && !m_file->open(QIODevice::ReadOnly)) {
                    emitFailed(QStringLiteral("the file to upload cannot be read"));
                    return nullptr;
                }
                if (m_file->size() != m_op.size) {
                    emitFailed(QStringLiteral("the file changed after it was read"));
                    return nullptr;
                }
                m_file->seek(0);
                return m_network->put(request, m_file.get());
        }
        return nullptr;
    }

    HostConfig m_host;
    Op m_op;
    AnswerSink* m_answer = nullptr;  ///< owned by the base class
    std::unique_ptr<QFile> m_file;
    bool m_aborted = false;

   public:
    OpResult result(bool ok, const QString& reason) const
    {
        OpResult out;
        out.ok = ok;
        out.status = replyStatusCode() > 0 ? replyStatusCode() : 0;
        out.body = answer();
        if (ok)
            return out;
        if (m_aborted) {
            out.error = QStringLiteral("aborted");
            return out;
        }
        const QByteArray body = m_errorResponse.isEmpty() ? answer() : m_errorResponse;
        out.error = describeStatus(out.status, body);
        if (out.status == 0)
            out.error += QStringLiteral(": ") + redactForLog(reason);  // a network error text can carry the address: through the redactor
        return out;
    }
};

class HeadOperation final : public Net::NetRequest {
   public:
    explicit HeadOperation(const QUrl& url)
    {
        setObjectName(QStringLiteral("SHAREDPACK-HEAD"));
        setNetwork(APPLICATION->network());
        setUrl(url);
        m_sink = std::make_unique<AnswerSink>(url.authority(), SmallBodyLimit);
    }

   private:
    QNetworkReply* getReply(QNetworkRequest& request) override { return m_network->head(request); }
};

UrlChecker::~UrlChecker()
{
    abort();
}

void UrlChecker::check(const QStringList& urls, Done done)
{
    m_pending = urls;
    m_resolved.clear();
    m_done = std::move(done);
    m_running = true;
    m_aborted = false;
    // Always answered from the event loop, also for an empty list.
    QTimer::singleShot(0, this, [this] { pump(); });
}

void UrlChecker::abort()
{
    m_aborted = true;
    m_pending.clear();
    const auto active = m_active;
    for (const auto& operation : active)
        operation->abort();
}

void UrlChecker::pump()
{
    while (!m_aborted && !m_pending.isEmpty() && m_active.size() < MaxConcurrent) {
        const QString url = m_pending.takeFirst();
        auto operation = makeShared<HeadOperation>(QUrl(url, QUrl::StrictMode));
        m_active.append(operation);
        HeadOperation* raw = operation.get();
        const auto finish = [this, raw, url](bool ok) {
            if (ok)
                m_resolved.insert(url);
            // Still inside the request's own signal: it is released from the event loop.
            QTimer::singleShot(0, this, [this, raw] {
                for (qsizetype i = 0; i < m_active.size(); ++i)
                    if (m_active.at(i).get() == raw) {
                        m_active.removeAt(i);
                        break;
                    }
                pump();
            });
        };
        connect(raw, &Task::succeeded, this, [finish] { finish(true); });
        connect(raw, &Task::failed, this, [finish] { finish(false); });
        connect(raw, &Task::aborted, this, [finish] { finish(false); });
        operation->start();
    }
    if (m_running && m_active.isEmpty() && (m_pending.isEmpty() || m_aborted)) {
        m_running = false;
        const auto done = m_done;
        if (done)
            done(m_aborted ? QSet<QString>() : m_resolved);
    }
}
S3Target::S3Target(HostConfig host, QObject* parent) : PublishTarget(parent), m_host(std::move(host))
{
    // Before any request can log an address or an error body that echoes a key.
    registerSecrets(m_host);
}

S3Target::~S3Target() = default;

void S3Target::run(const Op& op, Done done, Progress progress)
{
    auto operation = makeShared<S3Operation>(m_host, op);
    m_active.append(operation);
    S3Operation* raw = operation.get();

    if (progress)
        connect(raw, &Task::progress, this, [progress](qint64 current, qint64 total) { progress(current, total); });

    const auto finish = [this, raw, done](bool ok, const QString& reason) {
        done(raw->result(ok, reason));
        // The operation still touches itself after emitting: it is released from the event loop.
        QTimer::singleShot(0, this, [this, raw] {
            for (qsizetype i = 0; i < m_active.size(); ++i)
                if (m_active.at(i).get() == raw) {
                    m_active.removeAt(i);
                    break;
                }
        });
    };
    connect(raw, &Task::succeeded, this, [finish] { finish(true, {}); });
    connect(raw, &Task::failed, this, [finish](QString reason) { finish(false, reason); });
    connect(raw, &Task::aborted, this, [finish] { finish(false, QStringLiteral("aborted")); });

    // Started from the event loop so that a request that fails at once still reports through done() later, never inside run().
    QTimer::singleShot(0, this, [operation] { operation->start(); });
}

void S3Target::abortAll()
{
    const auto active = m_active;
    for (const auto& operation : active)
        operation->abort();
}

}  // namespace SharedPack
