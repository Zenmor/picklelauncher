// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackTasks.h"

#include <QDebug>
#include <QDir>
#include <QStorageInfo>
#include <algorithm>

#include "Application.h"
#include "InstanceList.h"
#include "SharedPackDisk.h"
#include "SharedPackLink.h"
#include "StringUtils.h"
#include "meta/Index.h"
#include "minecraft/Component.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/VanillaInstanceCreationTask.h"
#include "quickplay/QuickplayLoader.h"
#include "tasks/ConcurrentTask.h"

namespace SharedPack {

namespace {

const QString MinecraftUid = QStringLiteral("net.minecraft");
constexpr int MaxParallelDownloads = 8;

QString typeName()
{
    return QStringLiteral("pickleshared");
}

/// The loader a manifest asks for, None when it is plain Minecraft.
Quickplay::Loader loaderOf(const Manifest& manifest)
{
    for (const Component& component : manifest.components)
        for (const Quickplay::Loader loader : Quickplay::modLoaders())
            if (Quickplay::loaderUid(loader) == component.uid)
                return loader;
    return Quickplay::Loader::None;
}

QString versionOf(const Manifest& manifest, const QString& uid)
{
    for (const Component& component : manifest.components)
        if (component.uid == uid)
            return component.version;
    return {};
}

/// The id of an instance that already follows \a packId, empty when there is none.
QString instanceOfPack(const QString& packId)
{
    const InstanceList* instances = APPLICATION->instances();
    for (int i = 0; i < instances->count(); ++i) {
        const BaseInstance* instance = instances->at(i);
        if (instance->getManagedPackType() == typeName() && instance->getManagedPackID() == packId)
            return instance->id();
    }
    return {};
}

}  // namespace

void markAsSharedPack(MinecraftInstance* instance, const Manifest& manifest)
{
    instance->setManagedPack(typeName(), manifest.packId, manifest.name, QString::number(manifest.revision),
                             QObject::tr("Revision %1").arg(manifest.revision));
}

// ---------------------------------------------------------------- UpdateTask

UpdateTask::UpdateTask(QString instanceId, Invite invite, Manifest manifest)
    : m_instanceId(std::move(instanceId)), m_invite(std::move(invite)), m_manifest(std::move(manifest))
{}

UpdateTask::~UpdateTask()
{
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
    }
}

MinecraftInstance* UpdateTask::instance() const
{
    return dynamic_cast<MinecraftInstance*>(APPLICATION->instances()->getInstanceById(m_instanceId));
}

void UpdateTask::fail(const QString& reason)
{
    if (m_finished)
        return;
    m_finished = true;
    for (const auto& job : m_active)
        if (job->request)
            job->request->abort();
    m_active.clear();
    m_queue.clear();
    emitFailed(reason);
}

bool UpdateTask::abort()
{
    if (m_finished)
        return false;
    m_finished = true;
    for (const auto& job : m_active)
        if (job->request)
            job->request->abort();
    m_active.clear();
    m_queue.clear();
    emitAborted();
    return true;
}

void UpdateTask::executeTask()
{
    MinecraftInstance* inst = instance();
    if (!inst) {
        fail(tr("The instance of this pack no longer exists."));
        return;
    }
    if (inst->isRunning()) {
        fail(tr("Close the game before updating this pack."));
        return;
    }
    if (m_manifest.packId != m_invite.packId) {
        fail(tr("The host answered with a different pack."));
        return;
    }

    m_layout = layoutFor(inst->instanceRoot(), inst->gameRoot());
    const auto state = readStateOrFresh(m_layout, m_invite.packId);
    if (!state) {
        fail(tr("The saved state of this pack is damaged or belongs to another pack."));
        return;
    }
    m_state = *state;

    QStringList paths;
    for (const File& file : m_manifest.files)
        paths.append(file.path);
    for (auto it = m_state.installed.cbegin(); it != m_state.installed.cend(); ++it)
        paths.append(it.key());

    setStatus(tr("Checking the files..."));
    m_plan = planUpdate(m_state, m_manifest, scanDisk(m_layout.gameDir, paths));
    switch (m_plan.refusal) {
        case Plan::Refusal::None:
            break;
        case Plan::Refusal::OwnerRole:
            fail(tr("This is the owner's instance of the pack. It never receives updates."));
            return;
        case Plan::Refusal::WrongPack:
            fail(tr("The host answered with a different pack."));
            return;
        case Plan::Refusal::Backwards:
            fail(tr("This instance already has a newer revision than the host offers."));
            return;
    }

    const qint64 needed = requiredFreeBytes(m_plan, m_manifest);
    QDir().mkpath(m_layout.workDir);
    const QStorageInfo storage(m_layout.workDir);
    if (storage.isValid() && storage.bytesAvailable() < needed) {
        fail(tr("Not enough free disk space: %1 needed, %2 available.")
                 .arg(StringUtils::humanReadableFileSize(static_cast<double>(needed)),
                      StringUtils::humanReadableFileSize(static_cast<double>(storage.bytesAvailable()))));
        return;
    }

    // Blobs already downloaded and verified by an interrupted run are reused.
    for (const QString& sha : blobsNeeded(m_plan)) {
        const auto it = std::find_if(m_manifest.files.cbegin(), m_manifest.files.cend(), [&sha](const File& f) { return f.sha512 == sha; });
        if (it == m_manifest.files.cend())
            continue;  // cannot happen with a validated manifest and its own plan
        if (QFile::exists(m_layout.blobPath(sha)) && hashFile(m_layout.blobPath(sha)) == sha)
            continue;
        auto job = std::make_shared<Job>();
        job->file = *it;
        job->sources = sourcesFor(m_invite, *it);
        m_totalBytes += it->size;
        m_queue.append(job);
    }

    setStatus(tr("Downloading..."));
    pump();
}

void UpdateTask::updateProgress()
{
    qint64 current = m_doneBytes;
    for (const auto& job : m_active)
        current += job->current;
    setProgress(current, m_totalBytes);
}

void UpdateTask::pump()
{
    if (m_finished)
        return;
    while (m_active.size() < MaxParallelDownloads && !m_queue.isEmpty()) {
        auto job = m_queue.takeFirst();
        m_active.append(job);
        startJob(job);
    }
    if (m_active.isEmpty() && m_queue.isEmpty())
        afterDownloads();
}

void UpdateTask::startJob(const std::shared_ptr<Job>& job)
{
    const Source& source = job->sources.at(job->next);
    Limiter::Options options;
    options.allowedHosts = { source.url.authority().toLower() };
    options.maxBytes = job->file.size;
    options.expectedSize = job->file.size;
    options.expectedSha512 = job->file.sha512;

    job->current = 0;
    job->request = Request::make(m_invite, source.url, options, m_layout.blobPath(job->file.sha512));
    Request* request = job->request.get();

    const auto failedAttempt = [this, job](const QString& reason) {
        if (m_finished)
            return;
        // Only the file and the kind of source are logged: addresses of the pack's own host carry the token.
        qWarning() << "Shared pack: attempt" << job->next + 1 << "of" << job->sources.size() << "for" << job->file.path
                   << "failed:" << reason;
        job->request.reset();
        if (++job->next < job->sources.size()) {
            startJob(job);
            return;
        }
        fail(tr("Could not download %1 (%2).").arg(job->file.path, reason));
    };
    connect(request, &Task::progress, this, [this, job](qint64 current, qint64) {
        job->current = current;
        updateProgress();
    });
    connect(request, &Task::succeeded, this, [this, job] {
        if (m_finished)
            return;
        m_doneBytes += job->file.size;
        m_active.removeOne(job);
        job->request.reset();
        updateProgress();
        pump();
    });
    connect(request, &Task::failed, this, [failedAttempt, request](const QString& reason) {
        const QString violation = request->violation();
        failedAttempt(violation.isEmpty() ? reason : violation);
    });
    connect(request, &Task::aborted, this, [failedAttempt, request] {
        const QString violation = request->violation();
        failedAttempt(violation.isEmpty() ? tr("stopped") : violation);
    });
    request->start();
}

bool UpdateTask::setComponents(MinecraftInstance* inst)
{
    const QString minecraftVersion = versionOf(m_manifest, MinecraftUid);
    const Quickplay::Loader loader = loaderOf(m_manifest);
    PackProfile* profile = inst->getPackProfile();
    if (!profile || minecraftVersion.isEmpty())
        return false;

    // Same order as the Quickplay preparation: one loader only, the loader before Minecraft (changing the important Minecraft
    // component starts a resolve of the component list), then save; launching reloads and resolves them.
    for (const Quickplay::Loader other : Quickplay::modLoaders()) {
        if (other != loader)
            profile->remove(Quickplay::loaderUid(other));
    }
    if (loader == Quickplay::Loader::None || !Quickplay::usesIntermediary(loader))
        profile->remove(Quickplay::IntermediaryUid);

    if (loader != Quickplay::Loader::None) {
        const QString uid = Quickplay::loaderUid(loader);
        const QString version = versionOf(m_manifest, uid);
        if (profile->getComponentVersion(uid) != version)
            profile->setComponentVersion(uid, version);
        if (const auto component = profile->getComponent(uid))
            component->setEnabled(true);
    }
    if (profile->getComponentVersion(MinecraftUid) != minecraftVersion)
        profile->setComponentVersion(MinecraftUid, minecraftVersion, true);
    profile->saveNow();
    return true;
}

void UpdateTask::afterDownloads()
{
    MinecraftInstance* inst = instance();
    if (!inst || inst->isRunning()) {
        fail(inst ? tr("Close the game before updating this pack.") : tr("The instance of this pack no longer exists."));
        return;
    }
    setStatus(tr("Applying the update..."));
    if (!setComponents(inst)) {
        fail(tr("The pack has no usable Minecraft version."));
        return;
    }
    applyOnThread();
}

void UpdateTask::applyOnThread()
{
    m_threadResult = std::make_shared<ApplyResult>();
    const auto result = m_threadResult;
    const Layout layout = m_layout;
    const SharedPack::State state = m_state;
    const Plan plan = m_plan;

    // Copying large files must not block the window: the applier only touches files and uses no QObject of this thread.
    m_thread = QThread::create([result, layout, state, plan] { *result = applyPlan(layout, state, plan); });
    connect(m_thread, &QThread::finished, this, [this] {
        if (m_finished)
            return;
        m_result = *m_threadResult;
        if (!m_result.ok) {
            fail(m_result.error);
            return;
        }
        if (MinecraftInstance* inst = instance())
            markAsSharedPack(inst, m_manifest);
        QDir(m_layout.workDir).removeRecursively();
        m_finished = true;
        emitSucceeded();
    });
    m_thread->start();
}

// ------------------------------------------------------------------ JoinTask

JoinTask::JoinTask(Invite invite, Manifest manifest, bool autoUpdate)
    : m_invite(std::move(invite)), m_manifest(std::move(manifest)), m_autoUpdate(autoUpdate)
{}

void JoinTask::run(Task::Ptr task, std::function<void()> next)
{
    m_tasks.append(task);
    connect(task.get(), &Task::failed, this, [this](QString reason) { emitFailed(reason); });
    connect(task.get(), &Task::aborted, this, [this] { emitAborted(); });
    connect(task.get(), &Task::status, this, &Task::setStatus);
    connect(task.get(), &Task::progress, this, &Task::setProgress);
    connect(task.get(), &Task::succeeded, this, [next = std::move(next)] { next(); });
    task->start();
}

void JoinTask::executeTask()
{
    // A pack this launcher already follows is updated, not copied: a second instance would never be updated by the first one's link
    // and would drift. This is also how a member picks up a new invite after the owner rotated the link.
    m_instanceId = instanceOfPack(m_manifest.packId);
    if (!m_instanceId.isEmpty()) {
        updateExisting();
        return;
    }

    const Quickplay::Loader loader = loaderOf(m_manifest);
    auto minecraftList = APPLICATION->metadataIndex()->get(MinecraftUid);
    auto lists = Quickplay::loaderLists(loader);
    lists.append(minecraftList);
    if (std::any_of(lists.begin(), lists.end(), [](const auto& list) { return !list; })) {
        emitFailed(tr("The version metadata is not available."));
        return;
    }

    ConcurrentTask::Ptr loading(new ConcurrentTask(tr("Loading version lists")));
    bool anyToLoad = false;
    for (const auto& list : lists) {
        if (!list->isLoaded()) {
            loading->addTask(list->getLoadTask());
            anyToLoad = true;
        }
    }
    if (!anyToLoad) {
        createInstance();
        return;
    }
    setStatus(tr("Loading version lists..."));
    run(loading, [this] { createInstance(); });
}

void JoinTask::createInstance()
{
    const Quickplay::Loader loader = loaderOf(m_manifest);
    const QString minecraftVersion = versionOf(m_manifest, MinecraftUid);
    const BaseVersion::Ptr version = APPLICATION->metadataIndex()->get(MinecraftUid)->getVersion(minecraftVersion);
    if (!version) {
        emitFailed(tr("Minecraft %1 is not available in this launcher.").arg(minecraftVersion));
        return;
    }
    BaseVersion::Ptr loaderVersion;
    if (loader != Quickplay::Loader::None) {
        const QString loaderVersionName = versionOf(m_manifest, Quickplay::loaderUid(loader));
        loaderVersion = APPLICATION->metadataIndex()->get(Quickplay::loaderUid(loader))->getVersion(loaderVersionName);
        if (!loaderVersion) {
            emitFailed(tr("%1 %2 is not available in this launcher.").arg(Quickplay::loaderName(loader), loaderVersionName));
            return;
        }
    }

    auto* instances = APPLICATION->instances();
    auto* creation =
        loaderVersion ? new VanillaCreationTask(version, Quickplay::loaderUid(loader), loaderVersion) : new VanillaCreationTask(version);
    creation->setName(m_manifest.name);
    creation->setIcon("default");

    // The id is derived from the name by the instance list: learn it from the selection request sent when the instance is committed.
    auto idConnection = connect(instances, &InstanceList::instanceSelectRequest, this, [this](QString id) { m_instanceId = id; });
    setStatus(tr("Creating the instance..."));
    run(Task::Ptr(instances->wrapInstanceTask(creation)), [this, idConnection] {
        disconnect(idConnection);
        auto* instance = dynamic_cast<MinecraftInstance*>(APPLICATION->instances()->getInstanceById(m_instanceId));
        if (!instance) {
            emitFailed(tr("The instance could not be created."));
            return;
        }
        Link link;
        link.invite = m_invite;
        link.autoUpdate = m_autoUpdate;
        if (!writeLink(instance->instanceRoot(), link)) {
            emitFailed(tr("Cannot save the invite in the instance folder."));
            return;
        }
        // The pack is recorded at revision 0 until the first update has run.
        instance->setManagedPack(typeName(), m_manifest.packId, m_manifest.name, QStringLiteral("0"), tr("Not installed yet"));
        firstUpdate();
    });
}

void JoinTask::firstUpdate()
{
    auto* update = new UpdateTask(m_instanceId, m_invite, m_manifest);
    Task::Ptr task(update);
    run(task, [this, update] {
        m_result = update->result();
        emitSucceeded();
    });
}

void JoinTask::updateExisting()
{
    auto* update = new UpdateTask(m_instanceId, m_invite, m_manifest);
    Task::Ptr task(update);
    run(task, [this, update] {
        auto* instance = APPLICATION->instances()->getInstanceById(m_instanceId);
        if (!instance) {
            emitFailed(tr("The instance of this pack no longer exists."));
            return;
        }
        // The new invite replaces the old one only after the update worked: a refused or failed update (the owner's own instance, a
        // running game) leaves the link as it was. The member's auto-update choice stays.
        Link link = readLink(instance->instanceRoot()).value_or(Link{});
        link.invite = m_invite;
        if (!writeLink(instance->instanceRoot(), link)) {
            emitFailed(tr("Cannot save the invite in the instance folder."));
            return;
        }
        m_result = update->result();
        instance->setUpdateAvailable(false);  // the badge of the instance list: this update is what it announced
        emitSucceeded();
    });
}

}  // namespace SharedPack
