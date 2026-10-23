// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <functional>
#include <memory>
#include <optional>

#include "SharedPackManifest.h"
#include "SharedPackOwner.h"
#include "SharedPackPublishPlan.h"
#include "SharedPackTarget.h"

class QTimer;

namespace SharedPack {

/// What one publish needs: the manifest (revision already decided with nextRevision), the files on this computer that can serve as blobs,
/// the pack's folder token and what latest.json said when the owner started (empty = none).
struct PublishJob {
    Manifest manifest;
    QHash<QString, LocalBlob> blobs;  ///< by sha512
    QString token;
    std::optional<int> baseline;
};

/// Runs a publish or a rotate on a PublishTarget, in the order the pure plans say: HEAD the blobs, then the blobs (bounded concurrency,
/// each retried a few times on a network error or a 5xx), then revisions/<n>.json, then, after reading latest.json once more to see that
/// nobody else published meanwhile, latest.json. It never starts a later step after a failure, and an abort stops everything. One run at a
/// time. Lives on the event loop of its target; no widgets.
class Publisher : public QObject {
    Q_OBJECT
   public:
    static constexpr int DefaultMaxConcurrentPuts = 3;
    static constexpr int MaxConcurrentHeads = 4;
    static constexpr int MaxAttempts = 3;

    explicit Publisher(PublishTarget* target, HostConfig host, int maxConcurrentPuts = DefaultMaxConcurrentPuts, QObject* parent = nullptr);
    ~Publisher() override;

    /// Pause before the next attempt of an operation whose attempt number \a attempt (from 0) just failed with \a status. A 5xx or no answer
    /// is usually a moment's trouble (the Worker's free-plan CPU limit answers 503 on about one call in fifty): one to three seconds. A 429
    /// means the Worker's one-minute request window for this publisher is spent, so the pauses add up to longer than the window.
    static int retryDelayMs(int status, int attempt);
    /// Tests only: multiplies every retry pause (0 = try again at once). Not thread safe; set it before any publisher runs.
    static void setRetryDelayScale(double scale);

    /// Reads latest.json of \a token: nothing (ok, no revision) when the host has none. Answers through \a done.
    void readLatest(const QString& token, std::function<void(bool ok, std::optional<Latest> latest, const QString& error)> done);

    /// Checks which blobs the host has and plans the upload. With \a dryRun only planned() is emitted (the dialog's "what will be
    /// uploaded"), otherwise the plan is carried out. finished() ends it.
    void publish(const PublishJob& job, bool dryRun);

    /// Rotate Link: copies the pack (blobs, revision, latest.json last) from \a oldToken to \a newToken on the host, then deletes the old
    /// folder (latest.json first). If the copy fails nothing is deleted and the old link keeps working. A failed delete after a good copy
    /// still finishes ok with a warning in \a finished.
    void rotate(const Manifest& manifest, const QString& oldToken, const QString& newToken);

    void abort();
    bool busy() const { return m_busy; }

   signals:
    void planned(const SharedPack::UploadPlan& plan);
    /// Bytes of blobs uploaded so far of the plan's total, and what is going on.
    void progress(qint64 done, qint64 total);
    void status(const QString& text);
    /// \a error is empty on success, and may carry a warning then (rotate: the old folder could not be fully removed).
    void finished(bool ok, const QString& error);

   private:
    using ResultHandler = std::function<void(const OpResult&)>;

    void startOp(const Op& op, int attempt, ResultHandler done, PublishTarget::Progress progress);
    void checkBlobs(const PublishJob& job, bool dryRun);
    void runPlan(const UploadPlan& plan, std::function<Op(const Put&)> toOp, std::optional<int> baseline, bool recheck, QString token,
                 std::function<void()> whenDone);
    void pumpPlan();
    void startPut(int index);
    void putDone(int index, const OpResult& result);
    void runDeletes(QStringList keys, const QString& oldToken);
    void end(bool ok, const QString& error);

    PublishTarget* m_target;
    HostConfig m_host;
    int m_maxConcurrentPuts;
    bool m_busy = false;
    bool m_aborted = false;
    /// Operations waiting out a retry pause, with what to tell their caller if the publish is aborted meanwhile.
    QHash<QTimer*, std::function<void()>> m_retryWaits;

    // The plan in flight.
    std::unique_ptr<UploadSequencer> m_sequencer;
    std::function<Op(const Put&)> m_toOp;
    std::function<void()> m_planDone;
    std::optional<int> m_baseline;
    bool m_recheck = false;
    QString m_token;
    QHash<int, qint64> m_sent;  ///< per put: bytes sent so far
    qint64 m_total = 0;
    QString m_failure;
};

}  // namespace SharedPack
