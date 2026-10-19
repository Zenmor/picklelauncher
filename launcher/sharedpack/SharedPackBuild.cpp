// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackBuild.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <algorithm>

#include "SharedPackPath.h"

namespace SharedPack {

namespace {

BuildResult failure(const QString& error, QStringList skipped = {})
{
    BuildResult result;
    result.error = error;
    result.skipped = std::move(skipped);
    return result;
}

QString normalizedChangelog(QString text)
{
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    return text;
}

}  // namespace

bool isCachePath(const QString& path)
{
    static const QSet<QString> caches{ ".fabric", ".mixin.out", ".cache", "backups" };
    return caches.contains(path.section('/', 0, 0).toLower());
}

bool isPublishablePath(const QString& path, QString* why)
{
    QString reason;
    bool ok = isSafePath(path, &reason);
    if (ok && classify(path) == FileClass::Excluded) {
        ok = false;
        reason = QStringLiteral("saves, screenshots, logs, crash reports and mod metadata are never published");
    }
    if (ok && isCachePath(path)) {
        ok = false;
        reason = QStringLiteral("cache of the game or a mod loader");
    }
    if (!ok && why)
        *why = reason;
    return ok;
}

QString latestKey()
{
    return QStringLiteral("latest.json");
}

QString revisionKey(int revision)
{
    return QStringLiteral("revisions/") + QString::number(revision) + QStringLiteral(".json");
}

QString blobKey(const QString& sha512)
{
    return QStringLiteral("files/") + sha512;
}

QByteArray manifestToJson(const Manifest& manifest)
{
    QJsonArray components;
    for (const Component& component : manifest.components)
        components.append(QJsonObject{ { QStringLiteral("uid"), component.uid }, { QStringLiteral("version"), component.version } });

    QJsonArray files;
    for (const File& file : manifest.files) {
        QJsonObject item{ { QStringLiteral("path"), file.path },
                          { QStringLiteral("size"), static_cast<double>(file.size) },
                          { QStringLiteral("sha512"), file.sha512 } };
        if (!file.urls.isEmpty())
            item.insert(QStringLiteral("urls"), QJsonArray::fromStringList(file.urls));
        files.append(item);
    }

    QJsonObject object{ { QStringLiteral("formatVersion"), manifest.formatVersion },
                        { QStringLiteral("packId"), manifest.packId },
                        { QStringLiteral("name"), manifest.name },
                        { QStringLiteral("revision"), manifest.revision },
                        { QStringLiteral("components"), components },
                        { QStringLiteral("files"), files } };
    if (!manifest.changelog.isEmpty())
        object.insert(QStringLiteral("changelog"), manifest.changelog);
    if (!manifest.released.isEmpty())
        object.insert(QStringLiteral("released"), QJsonArray::fromStringList(manifest.released));
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray latestToJson(const Latest& latest)
{
    const QJsonObject object{ { QStringLiteral("formatVersion"), latest.formatVersion },
                              { QStringLiteral("packId"), latest.packId },
                              { QStringLiteral("revision"), latest.revision } };
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

BuildResult buildManifest(const BuildInput& input)
{
    Manifest manifest;
    manifest.packId = input.packId;
    manifest.name = input.name.trimmed();
    manifest.revision = input.revision;
    manifest.changelog = normalizedChangelog(input.changelog);
    manifest.components = input.components;

    QStringList skipped;
    QSet<QString> lowerPaths;
    qint64 total = 0;
    const PathSet unticked(input.unticked);
    for (const File& candidate : input.files) {
        QString why;
        if (!isPublishablePath(candidate.path, &why)) {
            skipped.append(candidate.path + QStringLiteral(": ") + why);
            continue;
        }
        if (unticked.covers(candidate.path)) {
            skipped.append(candidate.path + QStringLiteral(": not ticked"));
            continue;
        }
        if (candidate.size < 0 || candidate.size > Limits::MaxFileSize)
            return failure(QStringLiteral("%1 is larger than the %2 MB a pack can carry per file")
                               .arg(candidate.path)
                               .arg(Limits::MaxFileSize / (1024 * 1024)),
                           skipped);
        if (!isValidSha512(candidate.sha512))
            return failure(QStringLiteral("%1 has no valid checksum").arg(candidate.path), skipped);
        if (lowerPaths.contains(candidate.path.toLower()))
            return failure(QStringLiteral("two files differ only in upper and lower case: %1").arg(candidate.path), skipped);
        lowerPaths.insert(candidate.path.toLower());

        File file = candidate;
        file.urls.clear();
        for (const QString& url : candidate.urls)
            if (isAllowedUrl(url) && file.urls.size() < Limits::MaxUrlsPerFile && !file.urls.contains(url))
                file.urls.append(url);

        total += file.size;
        manifest.files.append(file);
    }
    // Unticked items that still exist: members keep their copy instead of losing it. A path that is not a safe path can never have been
    // installed on a member's side, so there is nothing to protect.
    QSet<QString> releasedLower;
    for (const QString& path : input.released) {
        if (!isSafePath(path) || releasedLower.contains(path.toLower()))
            continue;
        releasedLower.insert(path.toLower());
        manifest.released.append(path);
    }
    if (manifest.released.size() > Limits::MaxReleased)
        return failure(QStringLiteral("%1 items are not ticked, a pack can list at most %2: untick whole folders instead of single files")
                           .arg(manifest.released.size())
                           .arg(Limits::MaxReleased),
                       skipped);
    std::sort(manifest.released.begin(), manifest.released.end(),
              [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });

    if (manifest.files.size() > Limits::MaxFiles)
        return failure(QStringLiteral("a pack can have at most %1 files, this one has %2").arg(Limits::MaxFiles).arg(manifest.files.size()),
                       skipped);
    if (total > Limits::MaxTotalSize)
        return failure(QStringLiteral("a pack can be at most %1 GB, this one is larger").arg(Limits::MaxTotalSize / (1024 * 1024 * 1024)),
                       skipped);

    std::sort(manifest.files.begin(), manifest.files.end(), [](const File& a, const File& b) {
        const int order = a.path.compare(b.path, Qt::CaseInsensitive);
        return order != 0 ? order < 0 : a.path < b.path;
    });

    // What we publish is what a member parses: anything the parser would refuse (name, changelog, components, a path that is also a
    // folder, the document size) is found here, before a single byte is uploaded.
    const auto parsed = parseManifest(manifestToJson(manifest));
    if (!parsed)
        return failure(QStringLiteral("the pack would not be accepted by a member's launcher: %1").arg(parsed.error), skipped);

    BuildResult result;
    result.manifest = *parsed.value;
    result.skipped = skipped;
    return result;
}

}  // namespace SharedPack
