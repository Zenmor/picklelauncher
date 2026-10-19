// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>

#include "net/NetJob.h"
#include "quickplay/QuickplayLoader.h"
#include "quickplay/QuickplayPreset.h"
#include "quickplay/QuickplayResolver.h"
#include "tasks/Task.h"

namespace Quickplay {

/// Makes the mods folder of an instance contain exactly the mods of a preset for one Minecraft version and mod loader.
///
/// Mods come from Modrinth and CurseForge, each resolved on its own (see resolve()). If both providers end up with a file of the same
/// name, which is what the same mod picked twice looks like, only the first one is installed and the other one is reported.
///
/// Only files that Quickplay installed itself are ever touched: they are listed in a manifest next to the instance, so mods
/// added by the user are left alone. Files that are already present with the right hash are not downloaded again.
class ModInstallTask : public Task {
    Q_OBJECT
   public:
    /// `mods` are in the order given by Preset::installedMods(); an empty list only removes old Quickplay mods.
    ModInstallTask(QString minecraftVersion, Loader loader, QList<Mod> mods, QString modsDir, QString manifestPath);

    QStringList warnings() const override;

   protected:
    void executeTask() override;

   private:
    using Responses = std::function<void(const QList<QByteArray>&)>;

    void get(const QList<QUrl>& urls, Responses done);
    void resolveStep();
    void fetchPinnedVersions();
    void install();
    bool parseVersions(const ProjectRef& project, const QByteArray& data, bool isList);
    QString displayName(const ProjectRef& project) const;

    QString m_minecraftVersion;
    ModPlatform::ModLoaderTypes m_loaders;
    QList<Mod> m_mods;
    QString m_modsDir;
    QString m_manifestPath;

    ProviderVersions m_store;
    QSet<QString> m_blocked;  ///< keys of projects that only have files without a download link
    MultiResolveResult m_result;
    QStringList m_warnings;
    int m_rounds = 0;
    NetJob::Ptr m_job;
};

}  // namespace Quickplay
