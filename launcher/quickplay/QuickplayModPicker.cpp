// SPDX-License-Identifier: GPL-3.0-only

#include "QuickplayModPicker.h"

#include <QMessageBox>
#include <QTemporaryDir>
#include <algorithm>
#include <memory>

#include "Application.h"
#include "FileSystem.h"
#include "InstanceImportTask.h"
#include "ResourceDownloadTask.h"
#include "minecraft/PackProfile.h"
#include "settings/INISettingsObject.h"
#include "ui/dialogs/NewInstanceDialog.h"
#include "ui/dialogs/ResourceDownloadDialog.h"

namespace Quickplay {

PickInstance::PickInstance(SettingsObject* globalSettings, const QString& rootDir, Loader loader, const QList<Mod>& mods)
    : MinecraftInstance(globalSettings, std::make_unique<INISettingsObject>(FS::PathCombine(rootDir, "instance.cfg")), rootDir)
{
    // The profile is not marked as loaded, so it never writes mmc-pack.json. The version is not used by anything.
    if (const QString uid = loaderUid(loader); !uid.isEmpty())
        getPackProfile()->setComponentVersion(uid, QStringLiteral("0"));

    for (const auto& mod : mods)
        m_keys.insert(mod.key());
}

bool PickInstance::hasMod(ModPlatform::ResourceProvider provider, const QString& projectId) const
{
    return m_keys.contains(Mod{ projectId, {}, {}, provider }.key());
}

QList<Mod> pickMods(QWidget* parent, Loader loader, const QList<Mod>& existing, const QString& title)
{
    // Declared first, so that it is removed last: the instance's mod folder watches files in it.
    QTemporaryDir folder;
    if (!folder.isValid()) {
        QMessageBox::critical(parent, QObject::tr("Error"), QObject::tr("Could not create a temporary folder for the mod search."));
        return {};
    }

    PickInstance instance(APPLICATION->settings(), folder.path(), loader, existing);
    // The dialog knows what to do with a PickInstance: no Minecraft version filter, the preset's mods as installed, no review step.
    ResourceDownload::ModDownloadDialog dialog(parent, instance.loaderModList(), &instance);
    dialog.setWindowTitle(title);

    if (dialog.exec() != QDialog::Accepted)
        return {};

    QSet<QString> seen;
    for (const auto& mod : existing)
        seen.insert(mod.key());

    // A preset keeps projects only. The file version the dialog picked is of no interest: a version is chosen per Minecraft version
    // when the preset is installed.
    QList<Mod> picked;
    for (const auto& task : dialog.getTasks()) {
        const auto pack = task->getPack();
        const Mod mod{ pack->addonId.toString(), pack->name, pack->logoUrl, pack->provider };
        if (!mod.projectId.isEmpty() && !seen.contains(mod.key())) {
            seen.insert(mod.key());
            picked.append(mod);
        }
    }
    return picked;
}

QList<Mod> sameNameOtherProvider(const QList<Mod>& existing, const QList<Mod>& added)
{
    QList<Mod> clashes;
    QList<Mod> others = existing;
    for (const auto& mod : added) {
        const QString name = mod.name.trimmed();
        const bool clash = std::any_of(others.cbegin(), others.cend(), [&](const Mod& other) {
            return other.provider != mod.provider && other.name.trimmed().compare(name, Qt::CaseInsensitive) == 0;
        });
        if (clash)
            clashes.append(mod);
        others.append(mod);
    }
    return clashes;
}

std::optional<Mod> pickPack(QWidget* parent)
{
    // The dialog shows only the Modrinth and CurseForge pages when it is given this key (a hook marked "pickle-quickplay").
    NewInstanceDialog dialog(QString(), QString(), { { QStringLiteral("pickle-quickplay"), QStringLiteral("1") } }, parent);
    if (dialog.exec() != QDialog::Accepted)
        return std::nullopt;

    const QString page = dialog.selectedPageId();
    if (page != QStringLiteral("modrinth") && page != QStringLiteral("flame"))
        return std::nullopt;

    // Only the pack is of interest: the task the dialog made is not run, it tells which project was picked and under which name.
    std::unique_ptr<InstanceTask> task(dialog.extractTask());
    const auto* import = dynamic_cast<InstanceImportTask*>(task.get());
    if (!import)
        return std::nullopt;
    const QString id = import->extraInfo().value(QStringLiteral("pack_id"));
    if (id.isEmpty())
        return std::nullopt;
    const auto provider = page == QStringLiteral("flame") ? ModPlatform::ResourceProvider::FLAME : ModPlatform::ResourceProvider::MODRINTH;
    return Mod{ id, task->originalName(), {}, provider };
}
}  // namespace Quickplay
