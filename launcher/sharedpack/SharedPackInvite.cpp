// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackInvite.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace SharedPack {

namespace {

constexpr QLatin1String Scheme("picklepack:");

bool isAscii(QChar c)
{
    return c.unicode() < 128;
}

bool isSegmentChar(QChar c)
{
    return isAscii(c) && (c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_') || c == QLatin1Char('.'));
}

bool isValidPrefix(const QString& prefix)
{
    if (prefix.isEmpty())
        return true;
    if (prefix.size() > 128)
        return false;
    for (const QString& segment : prefix.split(QLatin1Char('/'))) {
        if (segment.isEmpty() || segment == QLatin1String(".") || segment == QLatin1String(".."))
            return false;
        for (QChar c : segment)
            if (!isSegmentChar(c))
                return false;
    }
    return true;
}

bool isValidBucket(const QString& bucket)
{
    if (bucket.size() < 3 || bucket.size() > 63)
        return false;
    for (QChar c : bucket)
        if (!isAscii(c) || !(c.isDigit() || (c.isLetter() && c.isLower()) || c == QLatin1Char('-') || c == QLatin1Char('.')))
            return false;
    return bucket.front().isLetterOrNumber() && bucket.back().isLetterOrNumber();
}

bool isValidRegion(const QString& region)
{
    if (region.isEmpty() || region.size() > 32)
        return false;
    for (QChar c : region)
        if (!isAscii(c) || !(c.isLower() || c.isDigit() || c == QLatin1Char('-')))
            return false;
    return true;
}

bool isValidKey(const QString& key)
{
    if (key.size() < 8 || key.size() > 128)
        return false;
    for (QChar c : key)
        if (c.unicode() < 0x21 || c.unicode() > 0x7e)
            return false;
    return true;
}

bool isValidHost(const QString& host)
{
    if (host.isEmpty() || host.size() > 253)
        return false;
    for (QChar c : host)
        if (!isAscii(c) || !(c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('.')))
            return false;
    return !host.startsWith(QLatin1Char('.')) && !host.endsWith(QLatin1Char('.'));
}

/// Normalizes and checks an endpoint; empty result = rejected. S3 endpoints have no path, Public ones may have a base path.
QString checkedEndpoint(const QString& text, bool allowPath)
{
    if (text.isEmpty() || text.size() > 512)
        return {};
    const QUrl url(text, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QLatin1String("https") || !isValidHost(url.host()))
        return {};
    if (!url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment())
        return {};
    if (url.port() == 0 || url.port() > 65535)
        return {};

    QString path = url.path();
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    if (!path.isEmpty()) {
        if (!allowPath || !isValidPrefix(path.mid(1)) || !path.startsWith(QLatin1Char('/')))
            return {};
    }
    return QStringLiteral("https://") + url.authority() + path;
}

QString stringField(const QJsonObject& object, const char* key, bool* ok)
{
    const QJsonValue value = object.value(QLatin1String(key));
    if (value.isUndefined() || value.isNull())
        return {};
    if (!value.isString()) {
        *ok = false;
        return {};
    }
    return value.toString();
}

Parsed<Invite> failed(Failure failure, const char* error)
{
    Parsed<Invite> result;
    result.failure = failure;
    result.error = QString::fromLatin1(error);
    return result;
}

}  // namespace

bool isValidToken(const QString& token)
{
    if (token.size() < 22 || token.size() > 128)
        return false;
    for (QChar c : token)
        if (!isAscii(c) || !(c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_')))
            return false;
    return true;
}

QStringList Invite::secrets() const
{
    QStringList result;
    for (const QString& secret : { token, accessKeyId, secretAccessKey })
        if (!secret.isEmpty())
            result.append(secret);
    return result;
}

QString Invite::host(int endpointIndex) const
{
    const QString base = (endpointIndex >= 0 && endpointIndex < endpoints.size()) ? endpoints.at(endpointIndex) : endpoint;
    return QUrl(base).authority();
}

QStringList Invite::allHosts() const
{
    QStringList result;
    if (!endpoint.isEmpty()) {
        const QString h = QUrl(endpoint).authority().toLower();
        if (!result.contains(h))
            result.append(h);
    }
    for (const QString& ep : endpoints) {
        const QString h = QUrl(ep).authority().toLower();
        if (!result.contains(h))
            result.append(h);
    }
    return result;
}

QString Invite::signedPathFor(const QString& relative, int endpointIndex) const
{
    QString path;
    if (mode() == Mode::S3) {
        path = QLatin1Char('/') + bucket;
    } else {
        const QString base = (endpointIndex >= 0 && endpointIndex < endpoints.size()) ? endpoints.at(endpointIndex) : endpoint;
        path = QUrl(base).path();
        while (path.endsWith(QLatin1Char('/')))
            path.chop(1);
    }
    if (!prefix.isEmpty())
        path += QLatin1Char('/') + prefix;
    return path + QLatin1Char('/') + token + QLatin1Char('/') + relative;
}

QUrl Invite::urlFor(const QString& relative, int endpointIndex) const
{
    QUrl url;
    url.setScheme(QStringLiteral("https"));
    url.setAuthority(host(endpointIndex));
    url.setPath(signedPathFor(relative, endpointIndex), QUrl::StrictMode);
    return url;
}

Parsed<Invite> parseInvite(const QString& input)
{
    const QString text = input.trimmed();
    if (text.size() > MaxInviteChars || !text.startsWith(Scheme))
        return failed(Failure::Corrupt, "not a shared pack invite");

    const QString rest = text.mid(Scheme.size());
    const qsizetype colon = rest.indexOf(QLatin1Char(':'));
    if (colon < 1 || colon > 4)
        return failed(Failure::Corrupt, "not a shared pack invite");
    bool numeric = false;
    const int version = rest.left(colon).toInt(&numeric);
    if (!numeric || version < 1)
        return failed(Failure::Corrupt, "not a shared pack invite");
    if (version > MaxInviteFormatVersion)
        return failed(Failure::TooNew, "this invite needs a newer launcher");

    const auto decoded = QByteArray::fromBase64Encoding(rest.mid(colon + 1).toLatin1(),
                                                        QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded)
        return failed(Failure::Corrupt, "the invite is damaged");

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(decoded.decoded, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return failed(Failure::Corrupt, "the invite is damaged");
    const QJsonObject object = document.object();

    bool ok = true;
    Invite invite;
    invite.version = version;
    invite.packId = stringField(object, "packId", &ok);
    invite.name = stringField(object, "name", &ok);
    invite.prefix = stringField(object, "prefix", &ok);
    invite.token = stringField(object, "token", &ok);

    if (!isValidPackId(invite.packId) || !isValidToken(invite.token) || !isValidPrefix(invite.prefix))
        return failed(Failure::Corrupt, "the invite is damaged");
    if (invite.name.size() > Limits::MaxNameChars)
        return failed(Failure::Corrupt, "the invite is damaged");

    if (version == 1) {
        invite.endpoint = stringField(object, "endpoint", &ok);
        invite.bucket = stringField(object, "bucket", &ok);
        invite.region = stringField(object, "region", &ok);
        invite.accessKeyId = stringField(object, "accessKeyId", &ok);
        invite.secretAccessKey = stringField(object, "secretAccessKey", &ok);
        if (!ok)
            return failed(Failure::Corrupt, "the invite is damaged");

        if (invite.mode() == Invite::Mode::S3) {
            if (invite.region.isEmpty())
                invite.region = QStringLiteral("auto");
            if (!isValidKey(invite.accessKeyId) || !isValidKey(invite.secretAccessKey) || !isValidBucket(invite.bucket) ||
                !isValidRegion(invite.region))
                return failed(Failure::Corrupt, "the invite is damaged");
            invite.endpoint = checkedEndpoint(invite.endpoint, false);
        } else {
            if (!invite.secretAccessKey.isEmpty() || !invite.bucket.isEmpty() || !invite.region.isEmpty())
                return failed(Failure::Corrupt, "the invite is damaged");
            invite.endpoint = checkedEndpoint(invite.endpoint, true);
        }
        if (invite.endpoint.isEmpty())
            return failed(Failure::Corrupt, "the invite must use an https address");
        invite.endpoints = { invite.endpoint };
    } else {
        // Version 2: Public only (no read keys, no bucket, no region)
        if (object.contains(QLatin1String("accessKeyId")) || object.contains(QLatin1String("secretAccessKey")) ||
            object.contains(QLatin1String("bucket")) || object.contains(QLatin1String("region"))) {
            return failed(Failure::Corrupt, "the invite is damaged");
        }

        if (object.value(QLatin1String("endpoints")).isArray()) {
            const QJsonArray arr = object.value(QLatin1String("endpoints")).toArray();
            for (const auto& val : arr) {
                if (!val.isString())
                    return failed(Failure::Corrupt, "the invite is damaged");
                const QString ep = checkedEndpoint(val.toString(), true);
                if (ep.isEmpty())
                    return failed(Failure::Corrupt, "the invite must use an https address");
                invite.endpoints.append(ep);
            }
        } else if (object.value(QLatin1String("endpoint")).isString()) {
            const QString ep = checkedEndpoint(object.value(QLatin1String("endpoint")).toString(), true);
            if (ep.isEmpty())
                return failed(Failure::Corrupt, "the invite must use an https address");
            invite.endpoints.append(ep);
        }
        if (invite.endpoints.isEmpty())
            return failed(Failure::Corrupt, "the invite is damaged");
        invite.endpoint = invite.endpoints.first();
    }

    Parsed<Invite> result;
    result.value = invite;
    return result;
}

namespace {

bool isInviteChar(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= u'A' && u <= u'Z') || (u >= u'a' && u <= u'z') || (u >= u'0' && u <= u'9') || u == u'-' || u == u'_' || u == u':' ||
           u == u'=';
}

}  // namespace

QString extractInvite(const QString& text)
{
    constexpr int MaxRuns = 256;  // a long invite wrapped at 20 columns is far below this
    constexpr qsizetype MaxTextChars = 64 * 1024;  // work is bounded by the text: a huge clipboard is not an invite message
    if (text.size() > MaxTextChars)
        return {};
    qsizetype from = 0;
    while ((from = text.indexOf(Scheme, from)) >= 0) {
        qsizetype pos = from;
        from += Scheme.size();  // the next search starts behind this occurrence
        // Words of the invite are appended one by one until the parser accepts the result: text after a line break belongs to the invite
        // only if the invite is not complete without it.
        QString candidate;
        for (int runs = 0; runs < MaxRuns && pos < text.size() && candidate.size() <= MaxInviteChars; ++runs) {
            qsizetype end = pos;
            while (end < text.size() && isInviteChar(text.at(end)))
                ++end;
            const QString run = text.mid(pos, end - pos);
            if (run.isEmpty() || (runs > 0 && run.startsWith(Scheme)))
                break;  // nothing more, or the next invite starts here
            candidate += run;
            if (parseInvite(candidate))
                return candidate;
            if (end < text.size() && !text.at(end).isSpace())
                break;  // a quote or punctuation ends the token
            while (end < text.size() && text.at(end).isSpace())
                ++end;
            pos = end;
        }
    }
    return {};
}

QString inviteToString(const Invite& invite)
{
    QJsonObject object;
    const auto put = [&object](const char* key, const QString& value) {
        if (!value.isEmpty())
            object.insert(QLatin1String(key), value);
    };
    put("packId", invite.packId);
    put("name", invite.name);
    put("prefix", invite.prefix);
    put("token", invite.token);

    int version = invite.version;
    if (invite.mode() == Invite::Mode::Public && (invite.endpoints.size() > 1 || invite.version == 2)) {
        version = 2;
        if (invite.endpoints.size() > 1) {
            QJsonArray arr;
            for (const QString& ep : invite.endpoints)
                arr.append(ep);
            object.insert(QLatin1String("endpoints"), arr);
        } else {
            put("endpoint", invite.endpoint);
        }
    } else {
        version = 1;
        put("endpoint", invite.endpoint);
        put("bucket", invite.bucket);
        put("region", invite.region);
        put("accessKeyId", invite.accessKeyId);
        put("secretAccessKey", invite.secretAccessKey);
    }

    const QByteArray json = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return Scheme + QString::number(version) + QLatin1Char(':') +
           QString::fromLatin1(json.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

}  // namespace SharedPack
