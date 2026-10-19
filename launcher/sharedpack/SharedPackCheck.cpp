// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackCheck.h"

#include <QDebug>

namespace SharedPack {

CheckTask::CheckTask(Invite invite, SharedPack::State state) : m_invite(std::move(invite)), m_packState(std::move(state)) {}

bool CheckTask::abort()
{
    if (m_finished)
        return false;
    m_finished = true;
    if (m_fetch)
        m_fetch->abort();
    emitAborted();
    return true;
}

void CheckTask::fail(CheckFailure failure, const QString& reason)
{
    if (m_finished)
        return;
    m_finished = true;
    m_failure = failure;
    emitFailed(reason);
}

void CheckTask::executeTask()
{
    if (m_packState.role == Role::Owner) {
        m_finished = true;
        emitSucceeded();  // the source instance is the truth, it never receives updates
        return;
    }
    setStatus(tr("Checking for updates..."));
    m_fetchingLatest = true;
    fetch(QStringLiteral("latest.json"), Limits::MaxLatestBytes, [this](const QByteArray& bytes) { onLatest(bytes); });
}

void CheckTask::fetch(const QString& relative, qint64 maxBytes, std::function<void(const QByteArray&)> done)
{
    auto task = makeShared<FetchBytesTask>(m_invite, relative, maxBytes);
    FetchBytesTask* raw = task.get();
    m_fetch = task;
    connect(raw, &Task::succeeded, this, [raw, done = std::move(done)] { done(raw->bytes()); });
    connect(raw, &Task::failed, this, [this, raw](QString reason) {
        const int status = raw->httpStatus();
        if (m_fetchingLatest && (status == 401 || status == 403 || status == 404))
            fail(CheckFailure::LinkInvalid, tr("This link is no longer valid."));
        else if (status == 0)  // no answer at all: a network or address problem, the raw text alone does not say that
            fail(CheckFailure::Other, tr("Could not reach the host: %1").arg(reason));
        else
            fail(CheckFailure::Other, reason);
    });
    connect(raw, &Task::aborted, this, [this] {
        if (m_finished)
            return;
        m_finished = true;
        emitAborted();
    });
    raw->start();
}

void CheckTask::onLatest(const QByteArray& bytes)
{
    m_fetchingLatest = false;
    const auto latest = parseLatest(bytes);
    if (!latest) {
        qWarning() << "SharedPack: latest.json rejected:" << latest.error;
        fail(latest.failure == Failure::TooNew ? CheckFailure::TooNew : CheckFailure::Other,
             latest.failure == Failure::TooNew ? tr("This pack needs a newer version of this launcher.")
                                               : tr("The pack information on the host is damaged."));
        return;
    }
    m_latestRevision = latest.value->revision;

    switch (checkForUpdate(m_packState, *latest.value)) {
        case UpdateCheck::Available:
            fetch(QStringLiteral("revisions/%1.json").arg(m_latestRevision), Limits::MaxManifestBytes,
                  [this](const QByteArray& manifestBytes) { onManifest(manifestBytes); });
            return;
        case UpdateCheck::WrongPack:
            fail(CheckFailure::Other, tr("The host answered with a different pack."));
            return;
        case UpdateCheck::UpToDate:
        case UpdateCheck::HostBehind:  // never move backwards: treated as nothing new
        case UpdateCheck::OwnerNeverUpdates:
            break;
    }
    m_finished = true;
    emitSucceeded();
}

void CheckTask::onManifest(const QByteArray& bytes)
{
    auto manifest = parseManifest(bytes);
    if (!manifest) {
        qWarning() << "SharedPack: revision manifest rejected:" << manifest.error;
        fail(manifest.failure == Failure::TooNew ? CheckFailure::TooNew : CheckFailure::Other,
             manifest.failure == Failure::TooNew ? tr("This pack needs a newer version of this launcher.")
                                                 : tr("The pack information on the host is damaged."));
        return;
    }
    // A host (or a half-finished publish) must not hand out another pack or revision than latest.json announced.
    if (manifest.value->packId != m_invite.packId || manifest.value->revision != m_latestRevision) {
        fail(CheckFailure::Other, tr("The host answered with a different revision."));
        return;
    }
    m_manifest = std::move(*manifest.value);
    m_finished = true;
    emitSucceeded();
}

}  // namespace SharedPack
