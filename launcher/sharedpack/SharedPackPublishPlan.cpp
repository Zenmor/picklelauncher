// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackPublishPlan.h"

#include <algorithm>

#include "SharedPackBuild.h"

namespace SharedPack {

int nextRevision(std::optional<int> hostLatest, int localRevision)
{
    const int base = std::max({ hostLatest.value_or(0), localRevision, 0 });
    return base >= Limits::MaxRevision ? 0 : base + 1;
}

Manifest rollbackManifest(const Manifest& old, int revision, const QString& changelog)
{
    Manifest manifest = old;
    manifest.revision = revision;
    manifest.changelog = changelog;
    return manifest;
}

bool latestUnchanged(std::optional<int> baseline, std::optional<int> now)
{
    return baseline == now;
}

bool needsBlob(const File& file)
{
    return file.urls.isEmpty();
}

QStringList blobsToCheck(const Manifest& manifest)
{
    QSet<QString> unique;
    for (const File& file : manifest.files)
        if (needsBlob(file))
            unique.insert(file.sha512);
    QStringList result(unique.cbegin(), unique.cend());
    result.sort();
    return result;
}

UploadPlan planUpload(const Manifest& manifest, const QSet<QString>& hostBlobs, const QHash<QString, LocalBlob>& localBlobs)
{
    UploadPlan plan;

    QSet<QString> seen;
    QList<Put> blobs;
    for (const File& file : manifest.files) {
        if (!needsBlob(file) || seen.contains(file.sha512))
            continue;
        seen.insert(file.sha512);
        if (hostBlobs.contains(file.sha512))
            continue;
        const auto local = localBlobs.constFind(file.sha512);
        if (local == localBlobs.constEnd()) {
            plan.error = QStringLiteral("%1 is not on the host and not on this computer").arg(file.path);
            return plan;
        }
        Put put;
        put.kind = Put::Kind::Blob;
        put.key = blobKey(file.sha512);
        put.file = local->path;
        put.size = file.size;
        put.sha512 = file.sha512;
        put.sha256 = local->sha256;
        blobs.append(put);
    }
    // Biggest first: the long uploads start early and the small ones fill the gaps.
    std::sort(blobs.begin(), blobs.end(), [](const Put& a, const Put& b) { return a.size != b.size ? a.size > b.size : a.sha512 < b.sha512; });

    for (const Put& put : blobs) {
        plan.blobBytes += put.size;
        ++plan.blobCount;
        plan.puts.append(put);
    }

    Put revision;
    revision.kind = Put::Kind::Revision;
    revision.key = revisionKey(manifest.revision);
    revision.body = manifestToJson(manifest);
    revision.size = revision.body.size();
    plan.puts.append(revision);

    Put latest;
    latest.kind = Put::Kind::Latest;
    latest.key = latestKey();
    latest.body = latestToJson(Latest{ FormatVersion, manifest.packId, manifest.revision });
    latest.size = latest.body.size();
    plan.puts.append(latest);
    return plan;
}

UploadSequencer::UploadSequencer(UploadPlan plan, int maxConcurrent) : m_plan(std::move(plan)), m_max(std::max(1, maxConcurrent)) {}

QList<int> UploadSequencer::next()
{
    QList<int> started;
    while (!m_failed && m_started < m_plan.puts.size() && m_inFlight < m_max) {
        const int index = m_started;
        // The revision and latest.json wait for everything before them: all of it must be in, none still running.
        if (m_plan.puts.at(index).kind != Put::Kind::Blob && m_completed != index)
            break;
        ++m_started;
        ++m_inFlight;
        started.append(index);
    }
    return started;
}

void UploadSequencer::done(int index, bool ok)
{
    Q_UNUSED(index)
    if (m_inFlight > 0)
        --m_inFlight;
    if (ok)
        ++m_completed;
    else
        m_failed = true;
}

RotatePlan planRotate(const Manifest& manifest)
{
    constexpr int MaxRevisionsToDelete = 1000;

    RotatePlan plan;
    for (const QString& sha512 : blobsToCheck(manifest))
        plan.copyKeys.append(blobKey(sha512));
    plan.copyKeys.append(revisionKey(manifest.revision));
    plan.copyKeys.append(latestKey());

    plan.deleteKeys.append(latestKey());
    for (int revision = std::max(1, manifest.revision - MaxRevisionsToDelete + 1); revision <= manifest.revision; ++revision)
        plan.deleteKeys.append(revisionKey(revision));
    for (const QString& sha512 : blobsToCheck(manifest))
        plan.deleteKeys.append(blobKey(sha512));
    return plan;
}

}  // namespace SharedPack
