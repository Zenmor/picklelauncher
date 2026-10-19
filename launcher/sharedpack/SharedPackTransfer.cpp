// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackTransfer.h"

#include "SharedPackSigV4.h"

namespace SharedPack {

namespace {

bool fail(QString* why, const char* text)
{
    if (why)
        *why = QString::fromLatin1(text);
    return false;
}

}  // namespace

bool Limiter::checkUrl(const QUrl& url, QString* why)
{
    const bool schemeOk = url.scheme() == QLatin1String("https") || (!m_options.requireHttps && url.scheme() == QLatin1String("http"));
    if (!url.isValid() || !schemeOk)
        return fail(why, "only https addresses are allowed");
    if (!url.userInfo().isEmpty())
        return fail(why, "an address with a user name is not allowed");
    if (!m_options.allowedHosts.contains(url.authority().toLower()))
        return fail(why, "the request was sent to a different host");
    return true;
}

void Limiter::reset()
{
    m_received = 0;
    m_failed = false;
    m_hash.reset();
}

bool Limiter::add(const QByteArray& data, QString* why)
{
    if (m_failed)
        return fail(why, "the download was already refused");
    m_received += data.size();
    if (m_received > m_options.maxBytes) {
        m_failed = true;
        return fail(why, "the download is larger than allowed");
    }
    if (m_options.expectedSize >= 0 && m_received > m_options.expectedSize) {
        m_failed = true;
        return fail(why, "the download is larger than the pack says");
    }
    m_hash.addData(data);
    return true;
}

bool Limiter::finish(int httpStatus, QString* why)
{
    if (m_failed)
        return fail(why, "the download was refused");
    if (httpStatus != 200)
        return fail(why, "the host did not answer with the file");
    if (m_options.expectedSize >= 0 && m_received != m_options.expectedSize)
        return fail(why, "the download is smaller than the pack says");
    if (!m_options.expectedSha512.isEmpty() && QString::fromLatin1(m_hash.result().toHex()) != m_options.expectedSha512)
        return fail(why, "the download does not match the pack (wrong checksum)");
    return true;
}

QList<Source> sourcesFor(const Invite& invite, const File& file)
{
    QList<Source> sources;
    for (const QString& url : file.urls) {
        if (isAllowedUrl(url))  // a validated manifest only has these, checked again because this decides where we connect
            sources.append({ QUrl(url, QUrl::StrictMode), false });
    }
    if (invite.endpoints.size() > 1) {
        for (int i = 0; i < invite.endpoints.size(); ++i) {
            sources.append({ invite.urlFor(QStringLiteral("files/") + file.sha512, i), true });
        }
    } else {
        sources.append({ invite.urlFor(QStringLiteral("files/") + file.sha512), true });
    }
    return sources;
}

HeaderList signedGetHeaders(const Invite& invite, const QUrl& url, const QDateTime& now)
{
    if (invite.mode() != Invite::Mode::S3 || !invite.allHosts().contains(url.authority().toLower()))
        return {};

    SigV4::Request request;
    request.method = QStringLiteral("GET");
    request.path = url.path();
    request.headers.insert(QStringLiteral("host"), url.authority());
    request.payloadHash = SigV4::emptyPayloadHash();
    request.time = now;
    request.region = invite.region;

    const SigV4::Signature signature = SigV4::sign({ invite.accessKeyId, invite.secretAccessKey }, request);
    if (signature.authorization.isEmpty())
        return {};
    return { { "x-amz-date", signature.amzDate.toLatin1() },
             { "x-amz-content-sha256", request.payloadHash.toLatin1() },
             { "Authorization", signature.authorization.toLatin1() } };
}

}  // namespace SharedPack
