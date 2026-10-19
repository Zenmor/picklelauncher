// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>
#include <functional>
#include <optional>

#include "QObjectPtr.h"
#include "SharedPackInvite.h"
#include "SharedPackManifest.h"
#include "SharedPackNet.h"
#include "SharedPackState.h"
#include "tasks/Task.h"

namespace SharedPack {

enum class CheckFailure {
    None,
    LinkInvalid,  ///< the host refuses or no longer has latest.json (401, 403, 404): the link was rotated or the pack removed
    TooNew,       ///< written by a newer launcher
    Other,        ///< network error, damaged or mismatching documents; the reason is in failReason()
};

/// Asks the host whether there is a newer revision than the one in \a state: latest.json first, and only when it announces a newer one
/// the revision manifest. Both are parsed and validated by the pure code, and the manifest must be the pack and revision latest.json
/// announced. Changes nothing on disk. Succeeds without a manifest when there is nothing to update (also for the owner, who never
/// updates). For a join pass a fresh member state (revision 0): any published revision is then "newer".
class CheckTask : public Task {
    Q_OBJECT
   public:
    CheckTask(Invite invite, SharedPack::State state);

    /// Set after success when a newer revision exists.
    const std::optional<Manifest>& manifest() const { return m_manifest; }
    bool updateAvailable() const { return m_manifest.has_value(); }
    /// Set after failure.
    CheckFailure failure() const { return m_failure; }

    bool canAbort() const override { return true; }
    bool abort() override;

   protected:
    void executeTask() override;

   private:
    void fetch(const QString& relative, qint64 maxBytes, std::function<void(const QByteArray&)> done);
    void onLatest(const QByteArray& bytes);
    void onManifest(const QByteArray& bytes);
    void fail(CheckFailure failure, const QString& reason);

    Invite m_invite;
    SharedPack::State m_packState;
    int m_latestRevision = 0;
    bool m_fetchingLatest = false;
    bool m_finished = false;
    shared_qobject_ptr<FetchBytesTask> m_fetch;
    std::optional<Manifest> m_manifest;
    CheckFailure m_failure = CheckFailure::None;
};

}  // namespace SharedPack
