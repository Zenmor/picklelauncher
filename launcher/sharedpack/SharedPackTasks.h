// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QList>
#include <QString>
#include <QThread>
#include <functional>
#include <memory>

#include "QObjectPtr.h"
#include "SharedPackApply.h"
#include "SharedPackInvite.h"
#include "SharedPackManifest.h"
#include "SharedPackNet.h"
#include "SharedPackPlan.h"
#include "SharedPackState.h"
#include "tasks/Task.h"

class MinecraftInstance;

namespace SharedPack {

/// Brings the instance \a instanceId to the revision of \a manifest. Nothing is touched before everything the plan needs is downloaded and
/// verified: the update either fails early (naming the file) or applies. Steps: plan from what is on disk, check free space, download
/// the blobs (the pack's Modrinth URLs first, then its own blob) into <instance>/sharedpack-tmp, set the Minecraft and loader versions,
/// apply on a worker thread, record the revision.
class UpdateTask : public Task {
    Q_OBJECT
   public:
    UpdateTask(QString instanceId, Invite invite, Manifest manifest);
    ~UpdateTask() override;

    const ApplyResult& result() const { return m_result; }
    const Manifest& manifest() const { return m_manifest; }
    bool canAbort() const override { return true; }
    bool abort() override;

   protected:
    void executeTask() override;

   private:
    struct Job {
        File file;
        QList<Source> sources;
        int next = 0;
        qint64 current = 0;
        shared_qobject_ptr<Request> request;
    };

    MinecraftInstance* instance() const;
    void fail(const QString& reason);
    void pump();
    void startJob(const std::shared_ptr<Job>& job);
    void updateProgress();
    void afterDownloads();
    bool setComponents(MinecraftInstance* instance);
    void applyOnThread();

    QString m_instanceId;
    Invite m_invite;
    Manifest m_manifest;
    Layout m_layout;
    SharedPack::State m_state;
    Plan m_plan;

    QList<std::shared_ptr<Job>> m_queue;
    QList<std::shared_ptr<Job>> m_active;
    qint64 m_totalBytes = 0;
    qint64 m_doneBytes = 0;
    bool m_finished = false;

    QThread* m_thread = nullptr;
    std::shared_ptr<ApplyResult> m_threadResult;
    ApplyResult m_result;
};

/// Creates a normal instance for a pack the member joined (right Minecraft and loader, the invite saved next to it) and runs the first
/// update on it. If the update fails the instance stays, and the next update of it finishes the job. `autoUpdate` is the member's choice
/// for a new instance; a pack the launcher already follows keeps the choice it has.
class JoinTask : public Task {
    Q_OBJECT
   public:
    JoinTask(Invite invite, Manifest manifest, bool autoUpdate);

    QString instanceId() const { return m_instanceId; }
    const ApplyResult& result() const { return m_result; }

   protected:
    void executeTask() override;

   private:
    void run(Task::Ptr task, std::function<void()> next);
    void createInstance();
    void firstUpdate();
    void updateExisting();

    Invite m_invite;
    Manifest m_manifest;
    bool m_autoUpdate;
    QString m_instanceId;
    QList<Task::Ptr> m_tasks;  ///< kept alive until this task is gone
    ApplyResult m_result;
};

/// Records a pack on an instance the way UpdateTask does: type, id, name and revision, so the instance page and playtime see it.
void markAsSharedPack(MinecraftInstance* instance, const Manifest& manifest);

}  // namespace SharedPack
