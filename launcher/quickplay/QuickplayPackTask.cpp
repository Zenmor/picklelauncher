// SPDX-License-Identifier: GPL-3.0-only

#include "QuickplayPackTask.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QUrl>
#include <memory>
#include <vector>

#include "Application.h"
#include "InstanceImportTask.h"
#include "InstanceList.h"
#include "Version.h"
#include "minecraft/MinecraftInstance.h"
#include "modplatform/ResourceAPI.h"
#include "net/ApiDownload.h"
#include "quickplay/QuickplayPack.h"
#include "settings/SettingsObject.h"

namespace Quickplay {

namespace {

const QString PackInstancesSetting = QStringLiteral("QuickplayPackInstances");

QJsonObject storedPackInstances()
{
    return QJsonDocument::fromJson(APPLICATION->settings()->get(PackInstancesSetting).toString().toUtf8()).object();
}

// The type the launcher writes into an instance it installed a pack for
QString managedType(ModPlatform::ResourceProvider provider)
{
    return provider == ModPlatform::ResourceProvider::FLAME ? QStringLiteral("flame") : QStringLiteral("modrinth");
}

ModPlatform::IndexedPack::Ptr packOf(const Mod& mod)
{
    auto pack = std::make_shared<ModPlatform::IndexedPack>();
    pack->addonId = mod.projectId;
    pack->name = mod.name;
    return pack;
}

}  // namespace

QStringList packInstanceIds()
{
    QStringList ids;
    const auto stored = storedPackInstances();
    for (auto it = stored.begin(); it != stored.end(); ++it)
        ids.append(it.value().toString());
    return ids;
}

void PackVersionsTask::executeTask()
{
    const auto url = providerApi(m_pack.provider).getInfoURL(m_pack.projectId);
    if (!url) {
        emitSucceeded();
        return;
    }

    m_job = makeShared<NetJob>(QStringLiteral("Quickplay pack versions"), APPLICATION->network());
    m_job->setAskRetry(false);
    auto [action, response] = Net::ApiDownload::makeByteArray(QUrl(*url));
    m_job->addNetAction(action);
    connect(m_job.get(), &NetJob::succeeded, this, [this, response] {
        m_versions = packMinecraftVersions(m_pack.provider, *response);
        emitSucceeded();
    });
    connect(m_job.get(), &NetJob::failed, this, [this](const QString& reason) {
        qWarning() << "Could not ask for the versions of" << m_pack.name << ":" << reason;
        emitSucceeded();
    });
    m_job->start();
}

void PackPrepareTask::executeTask()
{
    const auto& pack = m_preset.pack;
    ResourceAPI::VersionSearchArgs args{ .pack = packOf(pack),
                                         .mcVersions = std::vector<Version>{ Version(m_minecraftVersion) },
                                         .loaders = std::nullopt,
                                         .resourceType = ModPlatform::ResourceType::Modpack,
                                         .includeChangelog = false };
    const auto url = providerApi(pack.provider).getVersionsURL(args);
    if (!url) {
        emitFailed(tr("The versions of %1 cannot be asked for.").arg(pack.name));
        return;
    }

    setStatus(tr("Looking for %1 versions for Minecraft %2...").arg(pack.name, m_minecraftVersion));
    m_job = makeShared<NetJob>(QStringLiteral("Quickplay pack versions"), APPLICATION->network());
    auto [action, response] = Net::ApiDownload::makeByteArray(QUrl(*url));
    m_job->addNetAction(action);
    connect(m_job.get(), &NetJob::succeeded, this, [this, response] { install(*response); });
    connect(m_job.get(), &NetJob::failed, this, [this](const QString& reason) { emitFailed(reason); });
    connect(m_job.get(), &NetJob::aborted, this, [this] { emitAborted(); });
    m_job->start();
}

void PackPrepareTask::install(const QByteArray& versionList)
{
    const auto& pack = m_preset.pack;
    const auto versions = parsePackVersions(pack.provider, versionList);
    if (!versions) {
        emitFailed(tr("The answer about the versions of %1 was not understood.").arg(pack.name));
        return;
    }
    const auto version = pickPackVersion(*versions, m_minecraftVersion);
    if (!version) {
        emitFailed(tr("%1 has no version for Minecraft %2.").arg(pack.name, m_minecraftVersion));
        return;
    }
    const QString versionId = version->fileId.toString();

    auto* instances = APPLICATION->instances();
    auto stored = storedPackInstances();
    auto* existing = dynamic_cast<MinecraftInstance*>(instances->getInstanceById(stored.value(m_preset.id).toString()));

    QMap<QString, QString> extraInfo{ { "pack_id", pack.projectId }, { "pack_version_id", versionId } };
    if (existing) {
        if (existing->isRunning()) {
            emitFailed(tr("%1 is already running. Close it before switching version.").arg(pack.name));
            return;
        }
        if (existing->getManagedPackType() == managedType(pack.provider) && existing->getManagedPackID() == pack.projectId &&
            existing->getManagedPackVersionID() == versionId) {
            m_instanceId = existing->id();
            emitSucceeded();
            return;
        }
        extraInfo.insert("original_instance_id", existing->id());  // installs over it, so saves and options stay
    }

    // The name stays the modified one only: the original name would make the launcher look for an instance of the same pack by name,
    // and that could be one of the user's own. Everything the launcher would ask (update, untrusted files) is answered in advance.
    auto* import = new InstanceImportTask(QUrl(version->downloadUrl), true, nullptr, std::move(extraInfo));
    import->setName(pack.name);
    import->setIcon(QStringLiteral("default"));
    import->setGroup(QString());
    import->setConfirmUpdate(false);

    // The id is derived from the name by the instance list, learn it from the selection request sent when the instance is committed.
    auto idConnection = connect(instances, &InstanceList::instanceSelectRequest, this, [this](QString id) { m_instanceId = id; });
    setStatus(tr("Installing %1 %2...").arg(pack.name, version->version));

    m_import = Task::Ptr(instances->wrapInstanceTask(import));
    connect(m_import.get(), &Task::failed, this, [this](QString reason) { emitFailed(reason); });
    connect(m_import.get(), &Task::aborted, this, [this] { emitAborted(); });
    connect(m_import.get(), &Task::status, this, &Task::setStatus);
    connect(m_import.get(), &Task::progress, this, &Task::setProgress);
    connect(m_import.get(), &Task::succeeded, this, [this, idConnection] {
        disconnect(idConnection);
        if (!APPLICATION->instances()->getInstanceById(m_instanceId)) {
            emitFailed(tr("The instance of %1 could not be found.").arg(m_preset.pack.name));
            return;
        }
        auto instances = storedPackInstances();
        instances.insert(m_preset.id, m_instanceId);
        APPLICATION->settings()->set(PackInstancesSetting, QString::fromUtf8(QJsonDocument(instances).toJson(QJsonDocument::Compact)));
        emitSucceeded();
    });
    m_import->start();
}

}  // namespace Quickplay
