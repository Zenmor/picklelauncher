// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackScan.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>

#include "SharedPackBuild.h"
#include "SharedPackDisk.h"
#include "SharedPackManifest.h"

namespace SharedPack {

namespace {

constexpr qint64 ChunkBytes = 1024 * 1024;

struct Walker {
    QString root;
    const std::atomic_bool* cancel;
    const std::function<void(qint64)>& progress;
    ScanResult result;
    qint64 bytesRead = 0;
    const PathSet* unticked = nullptr;

    bool stopped() const { return !result.error.isEmpty(); }

    void hash(const QString& absolute, const QString& path, qint64 size)
    {
        QFile file(absolute);
        if (!file.open(QIODevice::ReadOnly)) {
            result.error = QStringLiteral("%1 cannot be read").arg(path);
            return;
        }
        QCryptographicHash sha512(QCryptographicHash::Sha512);
        QCryptographicHash sha256(QCryptographicHash::Sha256);
        qint64 total = 0;
        while (true) {
            if (cancel && cancel->load()) {
                result.cancelled = true;
                result.error = QStringLiteral("cancelled");
                return;
            }
            const QByteArray chunk = file.read(ChunkBytes);
            if (chunk.isEmpty())
                break;
            sha512.addData(chunk);
            sha256.addData(chunk);
            total += chunk.size();
            bytesRead += chunk.size();
            if (progress)
                progress(bytesRead);
        }
        if (file.error() != QFileDevice::NoError || total != size) {
            result.error = QStringLiteral("%1 changed or cannot be read completely").arg(path);
            return;
        }
        result.files.append({ path, absolute, size, QString::fromLatin1(sha512.result().toHex()), QString::fromLatin1(sha256.result().toHex()) });
    }

    void walk(const QString& absoluteDir, const QString& relativeDir)
    {
        const QFileInfoList entries =
            QDir(absoluteDir).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
        for (const QFileInfo& entry : entries) {
            if (stopped())
                return;
            const QString path = relativeDir.isEmpty() ? entry.fileName() : relativeDir + QLatin1Char('/') + entry.fileName();
            if (isLinkOrReparsePoint(entry.absoluteFilePath())) {
                result.skipped.append(path + QStringLiteral(": a link is never published"));
                continue;
            }
            if (entry.isDir()) {
                // A folder is judged by a file in it: this is how saves/, caches and mod metadata are kept out without being entered.
                QString why;
                if (!isPublishablePath(path + QStringLiteral("/x"), &why)) {
                    result.skipped.append(path + QStringLiteral("/: ") + why);
                    continue;
                }
                // The owner's choice, after the policy: a ticked folder the policy refuses was refused above. Not entered, not hashed.
                if (unticked && unticked->covers(path)) {
                    result.unticked.append(path);
                    continue;
                }
                walk(entry.absoluteFilePath(), path);
                continue;
            }
            if (!entry.isFile())
                continue;
            QString why;
            if (!isPublishablePath(path, &why)) {
                result.skipped.append(path + QStringLiteral(": ") + why);
                continue;
            }
            if (unticked && unticked->covers(path)) {
                result.unticked.append(path);
                continue;
            }
            hash(entry.absoluteFilePath(), path, entry.size());
        }
    }
};

}  // namespace

ScanResult scanGameFolder(const QString& gameDir, const std::atomic_bool* cancel, const std::function<void(qint64)>& progress,
                          const PathSet* unticked)
{
    Walker walker{ QDir::cleanPath(gameDir), cancel, progress, {}, 0, unticked };
    if (!QFileInfo(walker.root).isDir()) {
        walker.result.error = QStringLiteral("the game folder does not exist");
        return walker.result;
    }
    walker.walk(walker.root, {});
    std::sort(walker.result.files.begin(), walker.result.files.end(),
              [](const ScannedFile& a, const ScannedFile& b) { return a.path.compare(b.path, Qt::CaseInsensitive) < 0; });
    return walker.result;
}

QHash<QString, QString> parseModrinthUrls(const QByteArray& response, const QStringList& sha512s)
{
    QHash<QString, QString> urls;
    const QJsonDocument document = QJsonDocument::fromJson(response);
    if (!document.isObject())
        return urls;
    for (const QString& hash : sha512s) {
        const QJsonObject version = document.object().value(hash).toObject();
        for (const QJsonValue& value : version.value(QStringLiteral("files")).toArray()) {
            const QJsonObject file = value.toObject();
            if (file.value(QStringLiteral("hashes")).toObject().value(QStringLiteral("sha512")).toString() != hash)
                continue;
            const QString url = file.value(QStringLiteral("url")).toString();
            if (isAllowedUrl(url)) {
                urls.insert(hash, url);
                break;
            }
        }
    }
    return urls;
}

}  // namespace SharedPack
