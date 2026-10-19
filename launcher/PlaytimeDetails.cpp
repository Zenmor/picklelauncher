// SPDX-License-Identifier: GPL-3.0-only

#include "PlaytimeDetails.h"

#include <algorithm>

namespace PlaytimeDetails {

namespace {

bool played(const InstanceRecord& instance)
{
    return instance.lastLaunchMs > 0 || instance.seconds > 0;
}

Moment lastPlayed(const InstanceRecord& instance)
{
    if (instance.lastLaunchMs > 0)
        return { Moment::State::Known, instance.lastLaunchMs, false };
    return { played(instance) ? Moment::State::NotRecorded : Moment::State::NeverPlayed, 0, false };
}

}  // namespace

QString loaderName(const QString& loaderUid)
{
    if (loaderUid == QLatin1String("net.minecraftforge"))
        return QStringLiteral("Forge");
    if (loaderUid == QLatin1String("net.neoforged"))
        return QStringLiteral("NeoForge");
    if (loaderUid == QLatin1String("net.fabricmc.fabric-loader"))
        return QStringLiteral("Fabric");
    if (loaderUid == QLatin1String("org.quiltmc.quilt-loader"))
        return QStringLiteral("Quilt");
    if (loaderUid == QLatin1String("com.mumfrey.liteloader"))
        return QStringLiteral("LiteLoader");
    return {};
}

QString sourceName(const QString& packType)
{
    if (packType == QLatin1String("modrinth"))
        return QStringLiteral("Modrinth");
    if (packType == QLatin1String("flame"))
        return QStringLiteral("CurseForge");
    if (packType == QLatin1String("ftb"))
        return QStringLiteral("FTB");
    if (packType == QLatin1String("atlauncher"))
        return QStringLiteral("ATLauncher");
    return packType;
}

Moment firstPlayed(const InstanceRecord& instance)
{
    if (instance.firstLaunchMs > 0)
        return { Moment::State::Known, instance.firstLaunchMs, false };
    if (!played(instance))
        return { Moment::State::NeverPlayed, 0, false };

    // The folder is older than the first launch, so its date is a fair guess, unless it is younger than the last launch:
    // then the folder was made again (copy, import, update) and says nothing. Without a last launch nothing can be checked.
    if (instance.folderBirthMs > 0 && instance.lastLaunchMs > 0 && instance.folderBirthMs <= instance.lastLaunchMs)
        return { Moment::State::Known, instance.folderBirthMs, true };
    return { Moment::State::NotRecorded, 0, false };
}

Details build(const Input& input)
{
    Details details;
    details.name = input.name;
    details.sourceName = sourceName(input.packType);
    details.isPack = !input.packType.isEmpty();
    details.deletedSeconds = (std::max<qint64>)(input.deletedSeconds, 0);

    // one pass: the earliest and the latest known moment, and the weakest state when there is none
    bool anyFirstKnown = false;
    bool anyFirstNotRecorded = false;
    bool anyLastKnown = false;
    bool anyLastNotRecorded = false;
    for (const auto& instance : input.instances) {
        const auto first = firstPlayed(instance);
        if (first.state == Moment::State::Known) {
            // on a tie the recorded time wins over the guess
            if (!anyFirstKnown || first.ms < details.firstPlayed.ms ||
                (first.ms == details.firstPlayed.ms && details.firstPlayed.estimated))
                details.firstPlayed = first;
            anyFirstKnown = true;
        } else if (first.state == Moment::State::NotRecorded) {
            anyFirstNotRecorded = true;
        }

        const auto last = lastPlayed(instance);
        if (last.state == Moment::State::Known) {
            if (!anyLastKnown || last.ms > details.lastPlayed.ms)
                details.lastPlayed = last;
            anyLastKnown = true;
        } else if (last.state == Moment::State::NotRecorded) {
            anyLastNotRecorded = true;
        }

        details.instances.append(
            { instance.name, instance.packVersion, instance.minecraftVersion, loaderName(instance.loaderUid), instance.loaderVersion });
    }
    if (!anyFirstKnown && anyFirstNotRecorded)
        details.firstPlayed.state = Moment::State::NotRecorded;
    if (!anyLastKnown && anyLastNotRecorded)
        details.lastPlayed.state = Moment::State::NotRecorded;

    std::stable_sort(details.instances.begin(), details.instances.end(),
                     [](const InstanceBlock& a, const InstanceBlock& b) { return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
    return details;
}

}  // namespace PlaytimeDetails
