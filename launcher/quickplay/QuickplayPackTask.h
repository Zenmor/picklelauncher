// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

#include "net/NetJob.h"
#include "quickplay/QuickplayPreset.h"
#include "tasks/Task.h"

namespace Quickplay {

/// The ids of the instances of the modpack presets. They are hidden together with the shared Quickplay instance.
QStringList packInstanceIds();

/// Asks for the Minecraft versions a modpack has files for. It never fails: a pack that cannot be asked has none, so its preset has
/// no version to offer.
class PackVersionsTask : public Task {
    Q_OBJECT
   public:
    explicit PackVersionsTask(Mod pack) : m_pack(std::move(pack)) {}

    const Mod& pack() const { return m_pack; }
    const QSet<QString>& versions() const { return m_versions; }

   protected:
    void executeTask() override;

   private:
    Mod m_pack;
    QSet<QString> m_versions;
    NetJob::Ptr m_job;
};

/// Gets the instance of a modpack preset ready for a Minecraft version: finds the newest version of the pack for it and installs it
/// with the launcher's own import task, in place if the preset's instance exists already. Nothing is done if the instance has that
/// pack version. Launching is left to the caller (see instanceId()).
class PackPrepareTask : public Task {
    Q_OBJECT
   public:
    PackPrepareTask(QString minecraftVersion, Preset preset) : m_minecraftVersion(std::move(minecraftVersion)), m_preset(std::move(preset))
    {}

    /// Id of the pack's instance, valid once the task has succeeded.
    QString instanceId() const { return m_instanceId; }
    QStringList warnings() const override { return m_import ? m_import->warnings() : QStringList{}; }

   protected:
    void executeTask() override;

   private:
    void install(const QByteArray& versionList);

    QString m_minecraftVersion;
    Preset m_preset;
    QString m_instanceId;
    NetJob::Ptr m_job;
    Task::Ptr m_import;
};

}  // namespace Quickplay
