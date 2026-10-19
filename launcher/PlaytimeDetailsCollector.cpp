// SPDX-License-Identifier: GPL-3.0-only

#include "PlaytimeDetailsCollector.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <array>

#include "BaseInstance.h"
#include "InstanceList.h"
#include "minecraft/MinecraftInstance.h"
#include "minecraft/PackProfile.h"

namespace PlaytimeDetails {

namespace {

constexpr QLatin1String InstanceKeyPrefix{ "instance:" };
constexpr QLatin1String MinecraftUid{ "net.minecraft" };

// the component uids of the mod loaders the launcher knows, see loaderName()
const std::array<QLatin1String, 5> LoaderUids{ QLatin1String("net.minecraftforge"), QLatin1String("net.neoforged"),
                                               QLatin1String("net.fabricmc.fabric-loader"), QLatin1String("org.quiltmc.quilt-loader"),
                                               QLatin1String("com.mumfrey.liteloader") };

bool wanted(const QString& uid)
{
    return uid == MinecraftUid || std::find(LoaderUids.begin(), LoaderUids.end(), uid) != LoaderUids.end();
}

// uid -> version of Minecraft and of the loaders of an instance. Empty when nothing can be read.
QHash<QString, QString> componentVersions(BaseInstance& instance)
{
    QHash<QString, QString> versions;

    // A component list that is loaded already knows best (it may have changes that are not saved yet).
    if (auto* minecraft = dynamic_cast<MinecraftInstance*>(&instance)) {
        if (const auto* profile = minecraft->getPackProfile()) {
            const auto minecraftVersion = profile->getComponentVersion(MinecraftUid);
            if (!minecraftVersion.isEmpty()) {
                versions.insert(MinecraftUid, minecraftVersion);
                for (const auto& uid : LoaderUids) {
                    if (const auto version = profile->getComponentVersion(uid); !version.isEmpty())
                        versions.insert(uid, version);
                }
                return versions;
            }
        }
    }

    // Not loaded: PackProfile::load() would reset the model of the instance and reload() starts a resolve task, so the
    // file is read here instead. It is a small local file, there is no network and no waiting.
    QFile file(QDir(instance.instanceRoot()).filePath(QStringLiteral("mmc-pack.json")));
    if (!file.open(QIODevice::ReadOnly))
        return versions;
    for (const auto& item : QJsonDocument::fromJson(file.readAll()).object().value("components").toArray()) {
        const auto component = item.toObject();
        if (const auto uid = component.value("uid").toString(); wanted(uid))
            versions.insert(uid, component.value("version").toString());
    }
    return versions;
}

InstanceRecord recordOf(BaseInstance& instance)
{
    InstanceRecord record;
    record.name = instance.name();
    record.packVersion = instance.getManagedPackVersionName();

    const auto versions = componentVersions(instance);
    record.minecraftVersion = versions.value(MinecraftUid);
    // without the Minecraft version the components could not be read, so no loader is known either
    if (!record.minecraftVersion.isEmpty()) {
        for (const auto& uid : LoaderUids) {
            if (versions.contains(uid)) {
                record.loaderUid = uid;
                record.loaderVersion = versions.value(uid);
                break;
            }
        }
    }

    record.firstLaunchMs = instance.firstLaunch();
    record.lastLaunchMs = instance.lastLaunch();
    const auto born = QFileInfo(instance.instanceRoot()).birthTime();
    record.folderBirthMs = born.isValid() ? born.toMSecsSinceEpoch() : 0;
    record.seconds = instance.totalTimePlayed();
    return record;
}

}  // namespace

Details collect(InstanceList& instances, const QString& rowKey, const QString& rowName)
{
    Input input;
    input.name = rowName;

    if (rowKey.startsWith(InstanceKeyPrefix)) {
        auto* instance = instances.getInstanceById(rowKey.mid(InstanceKeyPrefix.size()));
        if (instance && QFileInfo::exists(instance->instanceRoot()))
            input.instances.append(recordOf(*instance));
        return build(input);
    }

    // a pack: "<type>:<id>", the type never contains a colon, the id might
    const auto colon = rowKey.indexOf(QLatin1Char(':'));
    if (colon <= 0)
        return build(input);
    const auto packType = rowKey.left(colon);
    const auto packId = rowKey.mid(colon + 1);
    input.packType = packType;
    if (input.name.isEmpty())
        input.name = packId;

    qint64 liveSeconds = 0;
    for (int i = 0; i < instances.count(); ++i) {
        auto* instance = instances.at(i);
        if (instance->getManagedPackType() != packType || instance->getManagedPackID() != packId ||
            !QFileInfo::exists(instance->instanceRoot()))
            continue;
        input.instances.append(recordOf(*instance));
        liveSeconds += instance->totalTimePlayed();
    }

    // rememberedPackPlayTime() is the time of the deleted instances plus the live ones (0 when nothing is remembered)
    input.deletedSeconds = instances.rememberedPackPlayTime(packType, packId) - liveSeconds;
    return build(input);
}

}  // namespace PlaytimeDetails
