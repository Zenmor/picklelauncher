// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackSigV4.h"

#include <QCryptographicHash>
#include <QLocale>
#include <QMessageAuthenticationCode>
#include <algorithm>

namespace SharedPack::SigV4 {

namespace {

// What is signed: the caller's headers (lower-case name, trimmed value) plus the two the signer owns. A QMap is sorted by name already.
QMap<QString, QString> headersToSign(const Request& request, const QString& amzDate)
{
    QMap<QString, QString> headers;
    for (auto it = request.headers.cbegin(); it != request.headers.cend(); ++it)
        headers.insert(it.key().toLower(), it.value().simplified());
    headers.insert(QStringLiteral("x-amz-date"), amzDate);
    headers.insert(QStringLiteral("x-amz-content-sha256"), request.payloadHash);
    return headers;
}

QByteArray hmacSha256(const QByteArray& key, const QByteArray& message)
{
    return QMessageAuthenticationCode::hash(message, key, QCryptographicHash::Sha256);
}

QString canonicalQuery(const Request& request)
{
    QList<QPair<QString, QString>> encoded;
    for (const auto& [name, value] : request.query)
        encoded.append({ uriEncode(name, true), uriEncode(value, true) });
    std::sort(encoded.begin(), encoded.end());  // by encoded name, then value
    QStringList parts;
    for (const auto& [name, value] : encoded)
        parts.append(name + QLatin1Char('=') + value);  // a sub-resource ("?lifecycle") has an empty value
    return parts.join(QLatin1Char('&'));
}

}  // namespace

QString sha256Hex(const QByteArray& data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

QString emptyPayloadHash()
{
    return sha256Hex(QByteArray());
}

QString uriEncode(const QString& text, bool encodeSlash)
{
    return QString::fromLatin1(text.toUtf8().toPercentEncoding(encodeSlash ? QByteArray() : QByteArray("/")));
}

QString signedHeaders(const Request& request)
{
    return headersToSign(request, QString()).keys().join(QLatin1Char(';'));
}

QString canonicalRequest(const Request& request, const QString& amzDate)
{
    const QMap<QString, QString> headers = headersToSign(request, amzDate);
    QString text = request.method + QLatin1Char('\n') + uriEncode(request.path, false) + QLatin1Char('\n') + canonicalQuery(request) +
                   QLatin1Char('\n');
    for (auto it = headers.cbegin(); it != headers.cend(); ++it)
        text += it.key() + QLatin1Char(':') + it.value() + QLatin1Char('\n');
    // The header block ends with its own newline, so a blank line follows it.
    return text + QLatin1Char('\n') + signedHeaders(request) + QLatin1Char('\n') + request.payloadHash;
}

Signature sign(const Credentials& credentials, const Request& request)
{
    Signature result;
    if (!request.headers.contains(QStringLiteral("host")) && !request.headers.contains(QStringLiteral("Host")))
        return result;  // S3 refuses a request whose host is not signed

    const QDateTime utc = request.time.toUTC();
    result.amzDate = QLocale::c().toString(utc, QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));  // not the system locale: its digits can differ
    const QString date = result.amzDate.left(8);
    const QString scope = date + QLatin1Char('/') + request.region + QLatin1Char('/') + request.service + QStringLiteral("/aws4_request");

    const QString stringToSign = QStringLiteral("AWS4-HMAC-SHA256\n") + result.amzDate + QLatin1Char('\n') + scope + QLatin1Char('\n') +
                                 sha256Hex(canonicalRequest(request, result.amzDate).toUtf8());

    QByteArray key = hmacSha256(("AWS4" + credentials.secretAccessKey).toUtf8(), date.toUtf8());
    key = hmacSha256(key, request.region.toUtf8());
    key = hmacSha256(key, request.service.toUtf8());
    key = hmacSha256(key, QByteArray("aws4_request"));
    const QString signature = QString::fromLatin1(hmacSha256(key, stringToSign.toUtf8()).toHex());

    result.authorization = QStringLiteral("AWS4-HMAC-SHA256 Credential=") + credentials.accessKeyId + QLatin1Char('/') + scope +
                           QStringLiteral(",SignedHeaders=") + signedHeaders(request) + QStringLiteral(",Signature=") + signature;
    return result;
}

}  // namespace SharedPack::SigV4
