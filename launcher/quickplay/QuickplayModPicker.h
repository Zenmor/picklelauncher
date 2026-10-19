// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <optional>

#include "minecraft/MinecraftInstance.h"
#include "quickplay/QuickplayLoader.h"
#include "quickplay/QuickplayPreset.h"

class QWidget;
class SettingsObject;

namespace Quickplay {

/// A throw-away instance that describes a preset to the launcher's "Download mods" dialog (ResourceDownload::ModDownloadDialog), which
/// needs an instance to read the mod loader from. It has the loader of the preset, no Minecraft version (a preset is resolved per
/// version at launch) and the mods of the preset as the installed ones. It lives in a folder of its own, is not in the instance list
/// and is never launched.
///
/// The dialog, its mod model and its filter widget recognise it with a dynamic_cast; all hooks are marked "pickle-quickplay" there.
class PickInstance : public MinecraftInstance {
   public:
    PickInstance(SettingsObject* globalSettings, const QString& rootDir, Loader loader, const QList<Mod>& mods);

    bool hasMod(ModPlatform::ResourceProvider provider, const QString& projectId) const;

   private:
    QSet<QString> m_keys;
};

/// Opens the launcher's "Download mods" dialog for a preset with the given loader and returns the projects the user chose, in the
/// order of the dialog. Projects in `existing` (same provider and project ID) are left out. Empty if the dialog was cancelled.
QList<Mod> pickMods(QWidget* parent, Loader loader, const QList<Mod>& existing, const QString& title);

/// Opens the launcher's New Instance dialog with only its Modrinth and CurseForge modpack pages and returns the pack the user chose (its
/// version is not kept, it is picked per Minecraft version at launch). Empty if the dialog was cancelled.
std::optional<Mod> pickPack(QWidget* parent);

/// The mods of `added` that `existing` (or an earlier mod of `added`) already has under the other provider, matched by name. Taking one
/// mod from both providers installs it once at best, so the user should know.
QList<Mod> sameNameOtherProvider(const QList<Mod>& existing, const QList<Mod>& added);

}  // namespace Quickplay
