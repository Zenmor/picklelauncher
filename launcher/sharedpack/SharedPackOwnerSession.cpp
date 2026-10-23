// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackOwnerSession.h"

#include <QDir>
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <iterator>

#include "Application.h"
#include "InstanceList.h"
#include "SharedPackBuild.h"
#include "SharedPackLink.h"
#include "SharedPackPath.h"
#include "SharedPackRedact.h"
#include "SharedPackRelayTarget.h"
#include "SharedPackS3Target.h"
#include "SharedPackTasks.h"
#include "StringUtils.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "modplatform/modrinth/ModrinthAPI.h"
#include "quickplay/QuickplayLoader.h"

namespace SharedPack {

namespace {

bool sameHost(const HostConfig& a, const HostConfig& b)
{
    return a.kind == b.kind && a.folder == b.folder && a.endpoint == b.endpoint && a.bucket == b.bucket && a.prefix == b.prefix &&
           a.region == b.region && a.writeKeyId == b.writeKeyId && a.writeSecret == b.writeSecret && a.readKeyId == b.readKeyId &&
           a.readSecret == b.readSecret && a.publisherToken == b.publisherToken && a.readBaseUrl == b.readBaseUrl &&
           a.fallbackReadUrl == b.fallbackReadUrl;
}

template <typename T>
void retire(std::unique_ptr<T>& pointer)
{
    // It may be the object whose signal is being handled right now: it goes away from the event loop.
    if (pointer)
        pointer.release()->deleteLater();
}

}  // namespace

OwnerSession::OwnerSession(QString instanceId, QObject* parent) : QObject(parent), m_instanceId(std::move(instanceId))
{
    load();
}

OwnerSession::~OwnerSession()
{
    m_cancel = true;
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
    }
}

MinecraftInstance* OwnerSession::instance() const
{
    return dynamic_cast<MinecraftInstance*>(APPLICATION->instances()->getInstanceById(m_instanceId));
}

QString OwnerSession::instanceName() const
{
    const auto* inst = instance();
    return inst ? inst->name() : QString();
}

QString OwnerSession::instanceRoot() const
{
    const auto* inst = instance();
    return inst ? inst->instanceRoot() : QString();
}

QString OwnerSession::gameRoot() const
{
    const auto* inst = instance();
    return inst ? inst->gameRoot() : QString();
}

void OwnerSession::load()
{
    const auto* inst = instance();
    if (!inst)
        return;
    m_link = readLink(inst->instanceRoot());
    if (!m_link)
        return;
    for (const QString& secret : m_link->invite.secrets())
        registerLogSecret(secret);
    // The owner role is the credentials' record naming THIS instance: a copy of the owner's instance has the link but is a member.
    m_record = findOwnerRecord(APPLICATION->dataRoot(), m_link->invite.packId);
    if (m_record && m_record->instanceId != m_instanceId)
        m_record.reset();
    m_isMember = !m_record;
}

bool OwnerSession::begin(Mode mode)
{
    if (m_busy) {
        emit failed(tr("Another step is still running."));
        return false;
    }
    const auto* inst = instance();
    if (!inst) {
        emit failed(tr("The instance is gone."));
        return false;
    }
    if (inst->isRunning()) {
        emit failed(tr("Close the game first: its files are still changing."));
        return false;
    }
    m_busy = true;
    m_mode = mode;
    m_cancel = false;
    m_warnings.clear();
    return true;
}

void OwnerSession::fail(const QString& reason)
{
    m_busy = false;
    m_mode = Mode::Idle;
    emit failed(reason);
}

void OwnerSession::makePublisher(const HostConfig& host)
{
    if (m_target)
        m_target->reset();  // a cancelled or failed publish must not make the next one answer "aborted"
    if (m_publisher && sameHost(m_host, host))
        return;
    retire(m_publisher);
    retire(m_target);
    retire(m_checker);
    m_host = host;
    registerSecrets(host);  // before the first request that could log an address or an error text
    int maxPuts = (host.kind == HostConfig::Kind::Relay) ? Publisher::DefaultMaxConcurrentPuts : 10;
    if (host.kind == HostConfig::Kind::Folder)
        m_target = std::make_unique<FolderTarget>(host.folder);
    else if (host.kind == HostConfig::Kind::Relay)
        m_target = std::make_unique<RelayTarget>(host);
    else
        m_target = std::make_unique<S3Target>(host);
    m_publisher = std::make_unique<Publisher>(m_target.get(), host, maxPuts);
    m_checker = std::make_unique<UrlChecker>();
    connect(m_publisher.get(), &Publisher::planned, this, [this](const UploadPlan& plan) { m_lastPlan = plan; });
    connect(m_publisher.get(), &Publisher::progress, this, &OwnerSession::progress);
    connect(m_publisher.get(), &Publisher::status, this, &OwnerSession::status);
    connect(m_publisher.get(), &Publisher::finished, this, &OwnerSession::onFinished);
}

// ---------------------------------------------------------------- prepare

void OwnerSession::prepare(const HostConfig& host, const QString& packName, const QString& changelog, const QStringList& unticked)
{
    if (m_isMember) {
        emit failed(tr("This instance follows a shared pack. Only its owner can publish it."));
        return;
    }
    if (!begin(Mode::Prepare))
        return;
    m_job.reset();
    m_changelog = changelog;
    m_unticked = unticked;
    m_name = packName.trimmed();

    HostConfig effective = host;
    if (m_record) {
        effective = m_record->host;
        m_invite = m_link->invite;
        if (!m_name.isEmpty())
            m_invite->name = m_name;
    } else {
        if (effective.kind == HostConfig::Kind::Relay && effective.publisherToken.isEmpty()) {
            const auto savedToken = findPublisherToken(APPLICATION->dataRoot(), effective.endpoint);
            if (savedToken)
                effective.publisherToken = *savedToken;
        }
        if (effective.kind == HostConfig::Kind::Relay && effective.packSecret.isEmpty()) {
            effective.packSecret = m_invite && sameHost(m_host, effective) ? m_host.packSecret : randomId(systemRandomBytes);
            if (effective.packSecret.isEmpty())
                return fail(tr("No random numbers available."));
        }
        QString error;
        // A second try after a failed first publish keeps the same pack and folder instead of leaving a stray one on the host.
        if (m_invite && sameHost(m_host, effective)) {
            m_invite->name = m_name;
        } else {
            m_invite = newPackInvite(effective, m_name, systemRandomBytes, &error);
            if (!m_invite)
                return fail(error);
        }
    }
    for (const QString& secret : m_invite->secrets())
        registerLogSecret(secret);
    if (m_name.isEmpty())
        m_name = m_invite->name;
    makePublisher(effective);
    startScan();
}

void OwnerSession::startScan()
{
    emit status(tr("Reading files..."));
    auto result = std::make_shared<ScanResult>();
    m_scan = result;
    const QString root = instance()->gameRoot();
    const PathSet unticked(m_unticked);
    m_thread = QThread::create([this, result, root, unticked] {
        *result = scanGameFolder(
            root, &m_cancel,
            [this](qint64 bytes) {
                QMetaObject::invokeMethod(
                    this, [this, bytes] { emit status(tr("Reading files... %1").arg(StringUtils::humanReadableFileSize(bytes))); },
                    Qt::QueuedConnection);
            },
            &unticked);
    });
    connect(m_thread, &QThread::finished, this, [this] {
        m_thread->deleteLater();
        m_thread = nullptr;
        onScanDone();
    });
    m_thread->start();
}

void OwnerSession::onScanDone()
{
    if (m_cancel)
        return fail(tr("Cancelled."));
    if (!*m_scan)
        return fail(tr("Could not read the game folder: %1").arg(m_scan->error));
    resolveUrls();
}

void OwnerSession::resolveUrls()
{
    QStringList hashes;
    QSet<QString> seen;
    for (const ScannedFile& file : m_scan->files)
        if (classify(file.path) == FileClass::PackWins && !seen.contains(file.sha512)) {
            seen.insert(file.sha512);
            hashes.append(file.sha512);
        }
    if (hashes.isEmpty())
        return onUrls({});

    emit status(tr("Looking up mods on Modrinth..."));
    ModrinthAPI api;
    auto [task, response] = api.currentVersions(hashes, QStringLiteral("sha512"));
    m_task = task;
    connect(task.get(), &Task::succeeded, this, [this, response, hashes] {
        const auto urls = parseModrinthUrls(*response, hashes);
        QTimer::singleShot(0, this, [this, urls] {  // the task is still inside its own signal
            m_task.reset();
            onUrls(urls);
        });
    });
    connect(task.get(), &Task::failed, this, [this] {
        m_warnings.append(tr("Modrinth could not be asked: every file will be uploaded."));
        QTimer::singleShot(0, this, [this] {
            m_task.reset();
            onUrls({});
        });
    });
    connect(task.get(), &Task::aborted, this, [this] {
        QTimer::singleShot(0, this, [this] {
            m_task.reset();
            fail(tr("Cancelled."));
        });
    });
    task->start();
}

void OwnerSession::onUrls(const QHash<QString, QString>& urls)
{
    if (m_cancel)
        return fail(tr("Cancelled."));
    m_urls = urls;
    const QStringList addresses = QSet<QString>(urls.cbegin(), urls.cend()).values();
    if (addresses.isEmpty())
        return onChecked({});
    emit status(tr("Checking the Modrinth links..."));
    m_checker->check(addresses, [this](const QSet<QString>& resolved) { onChecked(resolved); });
}

void OwnerSession::onChecked(const QSet<QString>& resolved)
{
    if (m_cancel)
        return fail(tr("Cancelled."));
    for (auto it = m_urls.begin(); it != m_urls.end();)
        it = resolved.contains(it.value()) ? std::next(it) : m_urls.erase(it);
    readHost();
}

void OwnerSession::readHost()
{
    emit status(tr("Asking the host..."));
    m_publisher->readLatest(m_invite->token, [this](bool ok, std::optional<Latest> latest, const QString& error) { onLatest(ok, latest, error); });
}

void OwnerSession::onLatest(bool ok, const std::optional<Latest>& latest, const QString& error)
{
    if (m_cancel)
        return fail(tr("Cancelled."));
    if (!ok)
        return fail(tr("Could not reach the host: %1").arg(error));
    if (latest && latest->packId != m_invite->packId)
        return fail(tr("The host already holds a different pack at this address."));
    buildJob(latest ? std::optional<int>(latest->revision) : std::nullopt);
}

void OwnerSession::buildJob(std::optional<int> hostRevision)
{
    auto* inst = instance();
    if (!inst)
        return fail(tr("The instance is gone."));
    const Layout layout = layoutFor(inst->instanceRoot(), inst->gameRoot());
    const auto state = readStateOrFresh(layout, m_invite->packId);
    if (!state)
        return fail(tr("The saved state of this instance is damaged."));
    const int revision = nextRevision(hostRevision, state->revision);
    if (revision == 0)
        return fail(tr("This pack has no revision numbers left."));

    BuildInput input;
    input.packId = m_invite->packId;
    input.name = m_name;
    input.revision = revision;
    input.changelog = m_changelog;
    input.unticked = m_unticked;
    input.released = m_scan->unticked;
    PackProfile* profile = inst->getPackProfile();
    const QString minecraft = profile->getComponentVersion(QStringLiteral("net.minecraft"));
    if (minecraft.isEmpty())
        return fail(tr("The instance has no Minecraft version."));
    input.components.append({ QStringLiteral("net.minecraft"), minecraft });
    for (const Quickplay::Loader loader : Quickplay::modLoaders()) {
        const QString uid = Quickplay::loaderUid(loader);
        const QString version = profile->getComponentVersion(uid);
        if (!version.isEmpty())
            input.components.append({ uid, version });
    }
    QHash<QString, ScannedFile> byPath;
    for (const ScannedFile& scanned : m_scan->files) {
        File file{ scanned.path, scanned.size, scanned.sha512, {} };
        if (m_urls.contains(scanned.sha512))
            file.urls = { m_urls.value(scanned.sha512) };
        input.files.append(file);
        byPath.insert(scanned.path, scanned);
    }

    const BuildResult built = buildManifest(input);
    if (!built)
        return fail(tr("The pack cannot be published: %1").arg(built.error));

    PublishJob job;
    job.manifest = *built.manifest;
    job.token = m_invite->token;
    job.baseline = hostRevision;
    int withUrls = 0;
    for (const File& file : job.manifest.files) {
        if (!needsBlob(file)) {
            ++withUrls;
            continue;
        }
        const ScannedFile& scanned = byPath.value(file.path);
        job.blobs.insert(file.sha512, LocalBlob{ scanned.absolute, scanned.size, scanned.sha256 });
    }
    m_job = job;
    if (auto* relay = dynamic_cast<RelayTarget*>(m_target.get()))
        relay->setJob(job);

    m_preview = Preview{};
    m_preview.manifest = job.manifest;
    m_preview.skipped = built.skipped + m_scan->skipped;
    m_preview.warnings = m_warnings;
    m_preview.publicUrlFiles = withUrls;
    m_preview.releasedCount = static_cast<int>(job.manifest.released.size());
    m_preview.previousRevision = hostRevision.value_or(0);
    m_preview.firstPublish = !m_record;
    m_lastPlan = UploadPlan{};
    m_publisher->publish(job, true);  // a dry run: HEAD the blobs, plan, change nothing
}

// ---------------------------------------------------------------- publish

void OwnerSession::publish()
{
    if (!m_job) {
        emit failed(tr("Nothing is ready to publish."));
        return;
    }
    if (!begin(Mode::Publish))
        return;
    if (!m_record) {
        // The keys are saved before the first byte goes out: a crash half way must not cost the owner the access to what was uploaded.
        if (!writeOwnerRecord(APPLICATION->dataRoot(), m_invite->packId, OwnerRecord{ m_instanceId, m_host }))
            return fail(tr("Could not save the access keys."));
        m_record = OwnerRecord{ m_instanceId, m_host };
    }
    m_publisher->publish(*m_job, false);
}

void OwnerSession::onFinished(bool ok, const QString& error)
{
    switch (m_mode) {
        case Mode::Prepare:
            if (!ok)
                return fail(error);
            m_preview.plan = m_lastPlan;
            m_busy = false;
            m_mode = Mode::Idle;
            emit prepared(m_preview);
            return;
        case Mode::Publish:
            if (!ok)
                return fail(error);
            finishPublish();
            return;
        case Mode::Rotate: {
            if (!ok)
                return fail(error);
            auto* inst = instance();
            QString warning = error;
            Link link = *m_link;
            link.invite = m_rotatedInvite;
            if (!inst || !writeLink(inst->instanceRoot(), link)) {
                // The pack has moved; the only copy of the new link is the one in the message.
                warning += (warning.isEmpty() ? QString() : QStringLiteral("\n")) +
                           tr("The new link could not be saved on this computer. Copy it now.");
            } else {
                m_link = link;
            }
            m_busy = false;
            m_mode = Mode::Idle;
            emit rotated(inviteToString(m_rotatedInvite), warning);
            return;
        }
        case Mode::Idle:
            return;
    }
}

void OwnerSession::finishPublish()
{
    auto* inst = instance();
    if (!inst)
        return fail(tr("The pack is published, but the instance is gone."));
    const Manifest& manifest = m_job->manifest;

    State state;
    state.packId = manifest.packId;
    state.role = Role::Owner;
    state.revision = manifest.revision;
    for (const File& file : manifest.files)
        state.installed.insert(file.path, file.sha512);

    Link link;
    link.invite = *m_invite;
    link.autoUpdate = false;
    if (!writeState(layoutFor(inst->instanceRoot(), inst->gameRoot()), state) || !writeLink(inst->instanceRoot(), link))
        return fail(tr("The pack is published, but this instance could not be marked as its owner. Publish again to retry."));
    markAsSharedPack(inst, manifest);
    m_link = link;
    m_record = OwnerRecord{ m_instanceId, m_host };
    m_isMember = false;

    const int revision = manifest.revision;
    m_job.reset();
    m_busy = false;
    m_mode = Mode::Idle;
    emit published(revision, inviteToString(*m_invite));
}

// ---------------------------------------------------------------- rotate

void OwnerSession::rotate()
{
    if (!m_record || !m_link) {
        emit failed(tr("This instance has not published a pack yet."));
        return;
    }
    if (!begin(Mode::Rotate))
        return;
    m_invite = m_link->invite;
    makePublisher(m_record->host);
    if (m_record->host.kind == HostConfig::Kind::Relay) {
        auto* relay = dynamic_cast<RelayTarget*>(m_target.get());
        if (!relay)
            return fail(tr("The Shared Packs Server is not available."));
        relay->rotateLink(m_invite->packId, m_invite->token, m_record->host.packSecret,
                          [this](bool ok, const QString& newReadToken, const QString& error) {
            if (!ok)
                return fail(tr("Could not rotate the link: %1").arg(error));
            m_rotatedInvite = *m_invite;
            m_rotatedInvite.token = newReadToken;
            registerLogSecret(newReadToken);
            onFinished(true, {});
        });
        return;
    }
    emit status(tr("Asking the host..."));
    m_publisher->readLatest(m_invite->token, [this](bool ok, std::optional<Latest> latest, const QString& error) {
        if (!ok)
            return fail(tr("Could not reach the host: %1").arg(error));
        if (!latest || latest->packId != m_invite->packId)
            return fail(tr("There is nothing published at the current link."));
        Op get;
        get.kind = Op::Kind::Get;
        get.key = m_host.keyFor(m_invite->token, revisionKey(latest->revision));
        get.maxGet = Limits::MaxManifestBytes;
        m_target->run(get, [this](const OpResult& result) {
            if (!result.ok)
                return fail(tr("Could not read the current revision: %1").arg(result.error));
            rotateWithManifest(result.body);
        });
    });
}

void OwnerSession::rotateWithManifest(const QByteArray& manifestJson)
{
    const auto manifest = parseManifest(manifestJson);
    if (!manifest || manifest.value->packId != m_invite->packId)
        return fail(tr("The current revision on the host is damaged."));
    const auto rotated = rotatedInvite(*m_invite, systemRandomBytes);
    if (!rotated)
        return fail(tr("No random numbers available."));
    m_rotatedInvite = *rotated;
    registerLogSecret(m_rotatedInvite.token);
    m_publisher->rotate(*manifest.value, m_invite->token, m_rotatedInvite.token);
}

void OwnerSession::abort()
{
    m_cancel = true;
    if (m_checker)
        m_checker->abort();
    if (m_publisher)
        m_publisher->abort();
    if (m_task)
        m_task->abort();
}

}  // namespace SharedPack
