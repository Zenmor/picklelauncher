// SPDX-License-Identifier: GPL-3.0-only

#include "QuickplayPrepareTask.h"

#include <algorithm>

#include "Application.h"
#include "FileSystem.h"
#include "InstanceList.h"
#include "meta/Index.h"
#include "minecraft/Component.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"
#include "minecraft/VanillaInstanceCreationTask.h"
#include "quickplay/QuickplayModInstaller.h"
#include "tasks/ConcurrentTask.h"

namespace Quickplay {

namespace {

const QString MinecraftUid = QStringLiteral("net.minecraft");
const QString InstanceIdSetting = QStringLiteral("QuickplayInstanceId");

}  // namespace

PrepareTask::PrepareTask(QString minecraftVersion, Preset preset)
    : m_minecraftVersion(std::move(minecraftVersion)), m_preset(std::move(preset))
{}

QStringList PrepareTask::warnings() const
{
    return m_modTask ? m_modTask->warnings() : QStringList{};
}

void PrepareTask::executeTask()
{
    loadMetadata();
}

// Runs a step and continues with `next` once it succeeded. Failure and abort of the step end this task.
void PrepareTask::run(Task::Ptr task, std::function<void()> next)
{
    m_tasks.append(task);
    connect(task.get(), &Task::failed, this, [this](QString reason) { emitFailed(reason); });
    connect(task.get(), &Task::aborted, this, [this] { emitAborted(); });
    connect(task.get(), &Task::status, this, &Task::setStatus);
    connect(task.get(), &Task::progress, this, &Task::setProgress);
    connect(task.get(), &Task::succeeded, this, [next = std::move(next)] { next(); });
    task->start();
}

void PrepareTask::loadMetadata()
{
    m_minecraftList = APPLICATION->metadataIndex()->get(MinecraftUid);
    auto lists = loaderLists(m_preset.loader);
    lists.append(m_minecraftList);

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

    const auto validate = [this] {
        const Loader loader = m_preset.loader;
        if (!m_minecraftList->hasVersion(m_minecraftVersion)) {
            emitFailed(tr("Minecraft %1 is not available.").arg(m_minecraftVersion));
            return;
        }
        if (loader != Loader::None) {
            if (!supportsMinecraft(loader, m_minecraftVersion)) {
                emitFailed(tr("%1 does not support Minecraft %2, so this preset is not available for it.")
                               .arg(loaderName(loader), m_minecraftVersion));
                return;
            }
            m_loaderVersion = pickLoaderVersion(loader, m_minecraftVersion);
            if (!m_loaderVersion) {
                emitFailed(tr("No %1 version is available for Minecraft %2.").arg(loaderName(loader), m_minecraftVersion));
                return;
            }
        }
        ensureInstance();
    };

    if (!anyToLoad) {
        validate();
        return;
    }
    setStatus(tr("Loading version lists..."));
    run(loading, validate);
}

void PrepareTask::ensureInstance()
{
    auto* instances = APPLICATION->instances();
    const QString savedId = APPLICATION->settings()->get(InstanceIdSetting).toString();

    if (auto* existing = dynamic_cast<MinecraftInstance*>(instances->getInstanceById(savedId))) {
        m_instanceId = existing->id();
        if (existing->isRunning()) {
            emitFailed(tr("Quickplay is already running. Close it before switching version or preset."));
            return;
        }
        switchComponents(existing);
        return;
    }

    // The new instance is created with the right version and loader already, so there is nothing to switch afterwards.
    const BaseVersion::Ptr version = m_minecraftList->getVersion(m_minecraftVersion);
    auto* creation =
        m_loaderVersion ? new VanillaCreationTask(version, loaderUid(m_preset.loader), m_loaderVersion) : new VanillaCreationTask(version);
    creation->setName(tr("Quickplay"));
    creation->setIcon("default");

    // The id is derived from the name by the instance list, learn it from the selection request sent when the instance is committed.
    auto idConnection = connect(instances, &InstanceList::instanceSelectRequest, this, [this](QString id) { m_instanceId = id; });

    setStatus(tr("Creating the Quickplay instance..."));
    run(Task::Ptr(instances->wrapInstanceTask(creation)), [this, instances, idConnection] {
        disconnect(idConnection);
        auto* created = dynamic_cast<MinecraftInstance*>(instances->getInstanceById(m_instanceId));
        if (!created) {
            emitFailed(tr("The Quickplay instance could not be created."));
            return;
        }
        APPLICATION->settings()->set(InstanceIdSetting, m_instanceId);
        installMods(created);
    });
}

void PrepareTask::switchComponents(MinecraftInstance* instance)
{
    PackProfile* profile = instance->getPackProfile();

    // Only one loader at a time: drop the others (remove() does nothing for a component that is not there). The intermediary mappings
    // are only a dependency of Fabric and Quilt, remove them explicitly instead of waiting for the resolve to do it.
    for (const Loader other : modLoaders()) {
        if (other != m_preset.loader)
            profile->remove(loaderUid(other));
    }
    if (!usesIntermediary(m_preset.loader))
        profile->remove(IntermediaryUid);

    // The loader goes before Minecraft: changing the important Minecraft component starts a resolve of the component list.
    if (m_loaderVersion) {
        const QString uid = loaderUid(m_preset.loader);
        if (profile->getComponentVersion(uid) != m_loaderVersion->descriptor())
            profile->setComponentVersion(uid, m_loaderVersion->descriptor());
        if (const auto component = profile->getComponent(uid))
            component->setEnabled(true);
    }
    if (profile->getComponentVersion(MinecraftUid) != m_minecraftVersion)
        profile->setComponentVersion(MinecraftUid, m_minecraftVersion, true);

    // Launching reloads the components from disk and resolves them again.
    profile->saveNow();
    installMods(instance);
}

void PrepareTask::installMods(MinecraftInstance* instance)
{
    m_modTask.reset(new ModInstallTask(m_minecraftVersion, m_preset.loader, m_preset.installedMods(), instance->modsRoot(),
                                       FS::PathCombine(instance->instanceRoot(), "quickplay.json")));
    run(m_modTask, [this] { emitSucceeded(); });
}

}  // namespace Quickplay
