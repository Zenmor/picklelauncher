// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

#include "SharedPackApply.h"
#include "SharedPackManifest.h"
#include "tasks/Task.h"

namespace SharedPack {

/// The start-up check. For every instance that follows a pack (it has a link file and is not the owner's source) it asks the host for a
/// newer revision, one instance after the other so a launcher with many packs does not open that many connections at once.
/// No widgets here: with auto-update on (and the game closed) the service runs the update itself, otherwise it reports the update and the
/// window decides (prompt with the changelog). All signals are emitted on the thread of the service.
class Service : public QObject {
    Q_OBJECT
   public:
    explicit Service(QObject* parent = nullptr);

    /// Starts a round over all instances. Does nothing while a round is still running.
    void checkAll();
    bool busy() const { return m_running; }

   signals:
    /// A newer revision exists and the member has to decide.
    void updateAvailable(const QString& instanceId, const SharedPack::Manifest& manifest);
    /// The host no longer accepts the link (rotated or removed): the member needs a new invite.
    void linkInvalid(const QString& instanceId);
    /// A network or format problem; \a reason is for the log and the status bar, never contains the invite.
    void checkFailed(const QString& instanceId, const QString& reason);
    /// \a result tells what the update left for the member to look at (.packnew files, skipped paths).
    void autoUpdated(const QString& instanceId, int revision, const SharedPack::ApplyResult& result);
    void autoUpdateFailed(const QString& instanceId, const QString& reason);
    /// The round is over.
    void finished();

   private:
    void next();
    void scheduleNext();
    void startAutoUpdate(const QString& instanceId, const Manifest& manifest);

    QStringList m_pending;
    Task::Ptr m_current;   ///< the check in flight, kept alive until the next one starts
    Task::Ptr m_updating;  ///< the auto-update in flight
    bool m_running = false;
};

}  // namespace SharedPack
