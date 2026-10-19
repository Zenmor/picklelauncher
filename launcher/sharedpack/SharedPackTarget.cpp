// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackTarget.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTimer>

#include "SharedPackSigV4.h"

namespace SharedPack {

namespace {

constexpr qint64 DefaultMaxGet = 8 * 1024 * 1024;
constexpr qint64 ChunkBytes = 1024 * 1024;

OpResult fail(int status, const QString& error)
{
    OpResult result;
    result.status = status;
    result.error = error;
    return result;
}

OpResult success(int status = 200)
{
    OpResult result;
    result.ok = true;
    result.status = status;
    return result;
}

/// Copies \a in to \a path (temporary file, then rename). \a expectedSize and \a expectedSha512 are checked when given: the file the owner
/// chose can change between planning and copying, and a blob under the wrong name would only fail on every member's computer.
OpResult copyInto(QIODevice& in, const QString& path, qint64 expectedSize, const QString& expectedSha512, const PublishTarget::Progress& progress)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return fail(500, QStringLiteral("could not create the folder"));
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly))
        return fail(500, QStringLiteral("could not write the file"));

    QCryptographicHash hash(QCryptographicHash::Sha512);
    qint64 total = 0;
    while (true) {
        const QByteArray chunk = in.read(ChunkBytes);
        if (chunk.isEmpty())
            break;
        if (out.write(chunk) != chunk.size()) {
            out.cancelWriting();
            return fail(500, QStringLiteral("could not write the file"));
        }
        hash.addData(chunk);
        total += chunk.size();
        if (progress)
            progress(total, expectedSize);
    }
    if (expectedSize >= 0 && total != expectedSize) {
        out.cancelWriting();
        return fail(409, QStringLiteral("the file changed while it was copied"));
    }
    if (!expectedSha512.isEmpty() && QString::fromLatin1(hash.result().toHex()) != expectedSha512) {
        out.cancelWriting();
        return fail(409, QStringLiteral("the file changed while it was copied"));
    }
    return out.commit() ? success() : fail(500, QStringLiteral("could not finish the file"));
}

bool isKeyChar(QChar c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

}  // namespace

bool isValidKey(const QString& key)
{
    if (key.isEmpty() || key.size() > 512)
        return false;
    for (const QString& segment : key.split('/')) {
        if (segment.isEmpty() || segment == QLatin1String(".") || segment == QLatin1String(".."))
            return false;
        for (QChar c : segment)
            if (!isKeyChar(c))
                return false;
        // Names Windows refuses matter for the folder target (a blob name is 128 characters, so isSafePath's segment limit does not fit).
        if (segment.endsWith(QLatin1Char('.')))
            return false;
        const QString stem = segment.section('.', 0, 0).toUpper();
        if (stem == QLatin1String("CON") || stem == QLatin1String("PRN") || stem == QLatin1String("AUX") || stem == QLatin1String("NUL"))
            return false;
        if (stem.size() == 4 && (stem.startsWith(QLatin1String("COM")) || stem.startsWith(QLatin1String("LPT"))) && stem[3] >= '0' && stem[3] <= '9')
            return false;
    }
    return true;
}
// ---------------------------------------------------------------- folder target

QString FolderTarget::pathFor(const QString& key) const
{
    return QDir::cleanPath(m_root) + QLatin1Char('/') + key;
}

OpResult FolderTarget::runSync(const Op& op) const
{
    return [&] {
        if (!isValidKey(op.key))
            return fail(400, QStringLiteral("not a valid key"));
        const QString path = pathFor(op.key);

        switch (op.kind) {
            case Op::Kind::Head:
                return QFileInfo(path).isFile() ? success() : fail(404, QStringLiteral("not found"));
            case Op::Kind::Get: {
                const QFileInfo info(path);
                if (!info.isFile())
                    return fail(404, QStringLiteral("not found"));
                const qint64 max = op.maxGet > 0 ? op.maxGet : DefaultMaxGet;
                if (info.size() > max)
                    return fail(413, QStringLiteral("the document is larger than allowed"));
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly))
                    return fail(500, QStringLiteral("could not read the file"));
                OpResult result = success();
                result.body = file.readAll();
                return result;
            }
            case Op::Kind::Put: {
                if (op.file.isEmpty()) {
                    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
                        return fail(500, QStringLiteral("could not create the folder"));
                    QSaveFile out(path);
                    if (!out.open(QIODevice::WriteOnly) || out.write(op.body) != op.body.size())
                        return fail(500, QStringLiteral("could not write the file"));
                    return out.commit() ? success() : fail(500, QStringLiteral("could not finish the file"));
                }
                QFile in(op.file);
                if (!in.open(QIODevice::ReadOnly))
                    return fail(404, QStringLiteral("the file to upload cannot be read"));
                return copyInto(in, path, op.size, op.sha512, {});
            }
            case Op::Kind::Delete: {
                if (QFileInfo::exists(path) && !QFile::remove(path))
                    return fail(500, QStringLiteral("could not delete the file"));
                // A removed pack folder should not leave empty folders behind (up to, never including, the root).
                const QString root = QFileInfo(m_root).absoluteFilePath();
                QString dir = QFileInfo(path).absolutePath();
                while (dir != root && dir.startsWith(root) && QDir(dir).isEmpty() && QDir().rmdir(dir))
                    dir = QFileInfo(dir).absolutePath();
                return success(204);
            }
            case Op::Kind::Copy: {
                if (!isValidKey(op.copyFrom))
                    return fail(400, QStringLiteral("not a valid key"));
                QFile in(pathFor(op.copyFrom));
                if (!in.exists())
                    return fail(404, QStringLiteral("not found"));
                if (!in.open(QIODevice::ReadOnly))
                    return fail(500, QStringLiteral("could not read the file"));
                return copyInto(in, path, -1, {}, {});
            }
        }
        return fail(400, QStringLiteral("unknown operation"));
    }();
}

void FolderTarget::run(const Op& op, Done done, Progress progress)
{
    const OpResult result = runSync(op);
    if (progress && result.ok && op.kind == Op::Kind::Put)
        progress(op.size, op.size);
    // Never call back from inside run(): the caller may still be setting up its bookkeeping.
    QTimer::singleShot(0, this, [done = std::move(done), result] { done(result); });
}

// ---------------------------------------------------------------- S3 request

S3Request buildS3Request(const HostConfig& host, const Op& op, const QDateTime& now)
{
    S3Request out;
    if (host.kind != HostConfig::Kind::S3 || !isValidKey(op.key) || (op.kind == Op::Kind::Copy && !isValidKey(op.copyFrom)))
        return out;
    const QUrl endpoint(host.endpoint);
    if (endpoint.scheme() != QLatin1String("https") || endpoint.host().isEmpty())
        return out;

    SigV4::Request request;
    request.path = QLatin1Char('/') + host.bucket + QLatin1Char('/') + op.key;
    request.region = host.region.isEmpty() ? QStringLiteral("auto") : host.region;
    request.time = now;
    request.headers.insert(QStringLiteral("host"), endpoint.authority());
    request.payloadHash = SigV4::emptyPayloadHash();

    QList<QPair<QByteArray, QByteArray>> extra;
    switch (op.kind) {
        case Op::Kind::Head:
            request.method = QStringLiteral("HEAD");
            break;
        case Op::Kind::Get:
            request.method = QStringLiteral("GET");
            break;
        case Op::Kind::Delete:
            request.method = QStringLiteral("DELETE");
            break;
        case Op::Kind::Copy: {
            // CopyObject: a PUT without a body whose source is named in a header; it must be signed like every x-amz header.
            request.method = QStringLiteral("PUT");
            const QString source = SigV4::uriEncode(QLatin1Char('/') + host.bucket + QLatin1Char('/') + op.copyFrom, false);
            request.headers.insert(QStringLiteral("x-amz-copy-source"), source);
            extra.append({ "x-amz-copy-source", source.toLatin1() });
            break;
        }
        case Op::Kind::Put: {
            request.method = QStringLiteral("PUT");
            QString contentType;
            if (op.file.isEmpty()) {
                request.payloadHash = SigV4::sha256Hex(op.body);
                contentType = QStringLiteral("application/json");
            } else {
                // The real hash, not UNSIGNED-PAYLOAD: it needs no special support on the host, and the host refuses a file that is not
                // the one that was hashed.
                if (op.sha256.size() != 64)
                    return out;
                request.payloadHash = op.sha256;
                contentType = QStringLiteral("application/octet-stream");
            }
            request.headers.insert(QStringLiteral("content-type"), contentType);
            extra.append({ "Content-Type", contentType.toLatin1() });
            break;
        }
    }

    const SigV4::Signature signature = SigV4::sign({ host.writeKeyId, host.writeSecret }, request);
    if (signature.authorization.isEmpty())
        return out;

    out.method = request.method;
    out.url.setScheme(QStringLiteral("https"));
    out.url.setAuthority(endpoint.authority());
    out.url.setPath(request.path, QUrl::StrictMode);
    out.headers = { { "x-amz-date", signature.amzDate.toLatin1() },
                    { "x-amz-content-sha256", request.payloadHash.toLatin1() },
                    { "Authorization", signature.authorization.toLatin1() } };
    out.headers.append(extra);
    out.valid = true;
    return out;
}

}  // namespace SharedPack
