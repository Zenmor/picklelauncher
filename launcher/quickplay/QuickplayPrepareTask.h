// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <functional>

#include "BaseVersion.h"
#include "meta/VersionList.h"
#include "quickplay/QuickplayPreset.h"
#include "tasks/Task.h"

class MinecraftInstance;

namespace Quickplay {

/// Gets the single shared Quickplay instance ready for a Minecraft version and preset: creates it if needed, switches its
/// version and mod loader (whichever the preset has) in place and installs the preset's mods. Launching is left to the caller (see
/// instanceId()).
class PrepareTask : public Task {
    Q_OBJECT
   public:
    PrepareTask(QString minecraftVersion, Preset preset);

    /// Id of the Quickplay instance, valid once the task has succeeded.
    QString instanceId() const { return m_instanceId; }
    QStringList warnings() const override;

   protected:
    void executeTask() override;

   private:
    void run(Task::Ptr task, std::function<void()> next);
    void loadMetadata();
    void ensureInstance();
    void switchComponents(MinecraftInstance* instance);
    void installMods(MinecraftInstance* instance);

    QString m_minecraftVersion;
    Preset m_preset;

    Meta::VersionList::Ptr m_minecraftList;
    BaseVersion::Ptr m_loaderVersion;  // null without a loader

    QString m_instanceId;
    QList<Task::Ptr> m_tasks;  // everything started so far, kept alive until this task is destroyed
    Task::Ptr m_modTask;
};

}  // namespace Quickplay
