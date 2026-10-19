// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QMap>
#include <QPair>
#include <QString>

/// AWS Signature Version 4 for S3-compatible hosts (Cloudflare R2 uses region "auto"). Pure: no network, no clock.
namespace SharedPack::SigV4 {

struct Credentials {
    QString accessKeyId;
    QString secretAccessKey;
};

struct Request {
    QString method;                        ///< "GET", "PUT", "HEAD"
    QString path;                          ///< absolute and NOT encoded, e.g. "/bucket/prefix/key" (path-style: the bucket is in the path)
    QList<QPair<QString, QString>> query;  ///< NOT encoded
    QMap<QString, QString> headers;        ///< the ones to sign besides x-amz-date and x-amz-content-sha256; must contain "host"
    QString payloadHash;                   ///< lower-case hex SHA-256 of the body ("" body: see emptyPayloadHash), or "UNSIGNED-PAYLOAD"
    QDateTime time;                        ///< any zone, used as UTC
    QString region;                        ///< "auto" on R2
    QString service = QStringLiteral("s3");
};

struct Signature {
    QString amzDate;        ///< value for the x-amz-date header
    QString authorization;  ///< value for the Authorization header; EMPTY when the request cannot be signed (no host header)
};

/// Signs \a request. The caller sends the request with x-amz-date = amzDate, x-amz-content-sha256 = request.payloadHash and Authorization.
Signature sign(const Credentials& credentials, const Request& request);

/// SHA-256 of nothing, the payload hash of every request without a body.
QString emptyPayloadHash();
QString sha256Hex(const QByteArray& data);

// The steps, public so the tests can check them against the intermediate values AWS publishes.
/// Percent-encodes UTF-8 bytes like AWS wants: only A-Z a-z 0-9 - . _ ~ stay, upper-case hex, and '/' stays unless \a encodeSlash.
QString uriEncode(const QString& text, bool encodeSlash);
QString canonicalRequest(const Request& request, const QString& amzDate);
QString signedHeaders(const Request& request);

}  // namespace SharedPack::SigV4
