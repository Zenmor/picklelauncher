// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackNet.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QNetworkReply>
#include <QNetworkRequest>

#include "Application.h"
#include "SharedPackRedact.h"

namespace SharedPack {

LimitedSink::LimitedSink(Limiter::Options options, QString file, std::function<void()> onViolation)
    : m_limiter(std::move(options)), m_file(std::move(file)), m_onViolation(std::move(onViolation))
{}

void LimitedSink::discard()
{
    if (m_part.isOpen())
        m_part.close();
    if (!m_file.isEmpty())
        QFile::remove(m_file + QStringLiteral(".part"));
}

Task::State LimitedSink::fail(const QString& why)
{
    const bool first = !m_violated;
    m_violated = true;
    m_fail_reason = why;
    discard();
    if (first && m_onViolation)
        m_onViolation();
    return Task::State::Failed;
}

Task::State LimitedSink::init(QNetworkRequest& request)
{
    // Runs for the first request and again for every redirect target.
    m_limiter.reset();
    m_bytes.clear();
    discard();

    QString why;
    if (!m_limiter.checkUrl(request.url(), &why))
        return fail(why);

    if (!m_file.isEmpty()) {
        if (!QDir().mkpath(QFileInfo(m_file).absolutePath()))
            return fail(QStringLiteral("could not create the folder for the download"));
        m_part.setFileName(m_file + QStringLiteral(".part"));
        if (!m_part.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return fail(QStringLiteral("could not open the temporary file"));
    }
    m_violated = false;
    m_fail_reason.clear();
    return Task::State::Running;
}

Task::State LimitedSink::write(QByteArray& data)
{
    QString why;
    if (!m_limiter.add(data, &why))
        return fail(why);
    if (m_file.isEmpty()) {
        m_bytes.append(data);
    } else if (m_part.write(data) != data.size()) {
        return fail(QStringLiteral("could not write the temporary file"));
    }
    return Task::State::Running;
}

Task::State LimitedSink::abort()
{
    discard();
    return Task::State::Failed;
}

Task::State LimitedSink::finalize(QNetworkReply& reply)
{
    QString why;
    if (!m_limiter.finish(reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), &why))
        return fail(why);
    if (!m_file.isEmpty()) {
        m_part.close();
        QFile::remove(m_file);
        if (!QFile::rename(m_file + QStringLiteral(".part"), m_file))
            return fail(QStringLiteral("could not move the finished download into place"));
    }
    return Task::State::Succeeded;
}

QList<Net::HeaderPair> SigV4HeaderProxy::headers(const QNetworkRequest& request) const
{
    QList<Net::HeaderPair> result;
    for (const auto& header : signedGetHeaders(m_invite, request.url(), QDateTime::currentDateTimeUtc()))
        result.append({ header.first, header.second });
    return result;
}

shared_qobject_ptr<Request> Request::make(const Invite& invite, const QUrl& url, Limiter::Options options, const QString& file)
{
    // Before anything can log the URL (NetRequest logs it on start, redirect, success and failure): every request goes through here.
    for (const QString& secret : invite.secrets())
        registerLogSecret(secret);

    auto request = makeShared<Request>();
    request->setUrl(url);
    request->setObjectName(QStringLiteral("SHAREDPACK"));
    request->setNetwork(APPLICATION->network());

    Request* raw = request.get();
    auto sink = std::make_unique<LimitedSink>(std::move(options), file, [raw] {
        // Stop the transfer: otherwise the rest of an oversized response would pile up in memory.
        QMetaObject::invokeMethod(
            raw,
            [raw] {
                if (raw->getState() == Task::State::Running)
                    raw->abort();
            },
            Qt::QueuedConnection);
    });
    request->m_sink_ptr = sink.get();
    request->m_sink = std::move(sink);
    request->addHeaderProxy(std::make_unique<SigV4HeaderProxy>(invite));
    return request;
}

FetchBytesTask::FetchBytesTask(Invite invite, QString relativePath, qint64 maxBytes)
    : m_invite(std::move(invite)), m_relative(std::move(relativePath)), m_maxBytes(maxBytes)
{}

void FetchBytesTask::executeTask()
{
    tryFetch(0);
}

void FetchBytesTask::tryFetch(int endpointIndex)
{
    m_endpointIndex = endpointIndex;
    Limiter::Options options;
    options.allowedHosts = { m_invite.host(endpointIndex).toLower() };
    options.maxBytes = m_maxBytes;

    m_request = Request::make(m_invite, m_invite.urlFor(m_relative, endpointIndex), options);
    auto* request = m_request.get();

    const auto finish = [this, request, endpointIndex](const QString& reason) {
        m_httpStatus = request->replyStatusCode() > 0 ? request->replyStatusCode() : 0;
        const bool canRetry = (m_httpStatus == 0 || m_httpStatus == 429 || m_httpStatus >= 500);
        if (canRetry && endpointIndex + 1 < m_invite.endpoints.size()) {
            tryFetch(endpointIndex + 1);
            return;
        }
        const QString violation = request->violation();
        emitFailed(violation.isEmpty() ? reason : violation);
    };
    connect(request, &Task::succeeded, this, [this, request] {
        m_bytes = request->bytes();
        m_httpStatus = 200;
        emitSucceeded();
    });
    connect(request, &Task::failed, this, finish);
    connect(request, &Task::aborted, this, [finish] { finish(QStringLiteral("the request was stopped")); });
    request->start();
}

}  // namespace SharedPack
