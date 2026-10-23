// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackPublisher.h"

#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <utility>

#include "SharedPackBuild.h"

namespace SharedPack {

namespace {

bool retryable(const OpResult& result)
{
    return result.status == 0 ? result.error != QLatin1String("aborted") : (result.status >= 500 || result.status == 429);
}

double g_retryDelayScale = 1.0;

QString what(Put::Kind kind)
{
    switch (kind) {
        case Put::Kind::Blob:
            return QObject::tr("a file");
        case Put::Kind::Revision:
            return QObject::tr("the revision");
        case Put::Kind::Latest:
            return QObject::tr("the pack's latest revision marker");
    }
    return {};
}

Put::Kind kindOfKey(const QString& key)
{
    if (key.startsWith(QLatin1String("files/")))
        return Put::Kind::Blob;
    return key == latestKey() ? Put::Kind::Latest : Put::Kind::Revision;
}

}  // namespace

Publisher::Publisher(PublishTarget* target, HostConfig host, int maxConcurrentPuts, QObject* parent)
    : QObject(parent), m_target(target), m_host(std::move(host)), m_maxConcurrentPuts(maxConcurrentPuts) {}

Publisher::~Publisher()
{
    m_aborted = true;
    if (m_busy && m_target)
        m_target->abortAll();
}

int Publisher::retryDelayMs(int status, int attempt)
{
    // Attempt 0 failed -> pause before attempt 1, attempt 1 failed -> pause before attempt 2. A 429 pauses 20 s then 40 s: the second
    // retry starts a full minute after the first refusal, past the Worker's one-minute window.
    static constexpr int transientMs[] = { 1000, 3000 };
    static constexpr int limitedMs[] = { 20000, 40000 };
    const int index = std::clamp(attempt, 0, 1);
    const int base = status == 429 ? limitedMs[index] : transientMs[index];
    return static_cast<int>(base * g_retryDelayScale);
}

void Publisher::setRetryDelayScale(double scale)
{
    g_retryDelayScale = std::max(0.0, scale);
}

void Publisher::abort()
{
    if (!m_busy)
        return;
    m_aborted = true;
    m_target->abortAll();
    // An operation that is only waiting to retry has nothing in flight for abortAll() to cancel: answer it now instead of when its pause ends.
    const auto waits = std::exchange(m_retryWaits, {});
    for (auto it = waits.begin(); it != waits.end(); ++it) {
        it.key()->stop();
        it.key()->deleteLater();
        QTimer::singleShot(0, this, it.value());
    }
}

void Publisher::startOp(const Op& op, int attempt, ResultHandler done, PublishTarget::Progress progress)
{
    QPointer<Publisher> self(this);
    m_target->run(
        op,
        [self, op, attempt, done, progress](const OpResult& result) {
            if (!self)
                return;  // the publisher is gone: nobody is waiting for this
            if (result.ok || self->m_aborted || !retryable(result) || attempt + 1 >= MaxAttempts) {
                done(result);
                return;
            }
            auto* timer = new QTimer(self.data());
            timer->setSingleShot(true);
            self->m_retryWaits.insert(timer, [done] {
                OpResult aborted;
                aborted.error = QStringLiteral("aborted");
                done(aborted);
            });
            QObject::connect(timer, &QTimer::timeout, self.data(), [self, timer, op, attempt, done, progress] {
                self->m_retryWaits.remove(timer);
                timer->deleteLater();
                self->startOp(op, attempt + 1, done, progress);
            });
            timer->start(retryDelayMs(result.status, attempt));
        },
        progress);
}

void Publisher::readLatest(const QString& token, std::function<void(bool, std::optional<Latest>, const QString&)> done)
{
    Op op;
    op.kind = Op::Kind::Get;
    op.key = m_host.keyFor(token, latestKey());
    op.maxGet = Limits::MaxLatestBytes;
    startOp(op, 0, [done](const OpResult& result) {
        if (result.absent())
            return done(true, std::nullopt, {});
        if (!result.ok)
            return done(false, std::nullopt, result.error);
        const auto parsed = parseLatest(result.body);
        if (!parsed)
            return done(false, std::nullopt, QObject::tr("latest.json on the host is damaged"));
        done(true, *parsed.value, {});
    }, {});
}

void Publisher::end(bool ok, const QString& error)
{
    // A failed publish must not keep the host's pack reserved until its session expires; a cancel has already released it.
    if (!ok && !m_aborted && m_target)
        m_target->releaseSession();
    m_busy = false;
    emit finished(ok, error);
}

// ---------------------------------------------------------------- publish

void Publisher::publish(const PublishJob& job, bool dryRun)
{
    if (m_busy) {
        emit finished(false, tr("Another operation is still running."));
        return;
    }
    m_busy = true;
    m_aborted = false;
    m_failure.clear();
    emit status(tr("Preparing..."));
    QPointer<Publisher> self(this);
    m_target->prepare(dryRun, [this, self, job, dryRun](bool ok, const QString& error) {
        if (!self)
            return;
        if (!ok)
            return end(false, error);
        checkBlobs(job, dryRun);
    });
}

void Publisher::checkBlobs(const PublishJob& job, bool dryRun)
{
    struct HeadState {
        QStringList pending;
        QSet<QString> present;
        int inFlight = 0;
        bool failed = false;
        bool done = false;
    };
    auto state = std::make_shared<HeadState>();
    state->pending = blobsToCheck(job.manifest);
    emit status(tr("Checking what the host already has..."));

    auto pump = std::make_shared<std::function<void()>>();
    QPointer<Publisher> self(this);
    *pump = [this, self, state, pump, job, dryRun] {
        if (!self || state->done)
            return;
        while (!state->failed && !m_aborted && state->inFlight < MaxConcurrentHeads && !state->pending.isEmpty()) {
            const QString sha512 = state->pending.takeFirst();
            ++state->inFlight;
            Op op;
            op.kind = Op::Kind::Head;
            op.key = m_host.keyFor(job.token, blobKey(sha512));
            startOp(op, 0, [this, self, state, pump, sha512](const OpResult& result) {
                if (!self)
                    return;
                --state->inFlight;
                if (result.ok)
                    state->present.insert(sha512);
                else if (!result.absent()) {
                    state->failed = true;
                    m_failure = result.error;
                }
                (*pump)();
            }, {});
        }
        if (state->inFlight > 0 || (!state->failed && !m_aborted && !state->pending.isEmpty()))
            return;
        state->done = true;
        QTimer::singleShot(0, [pump] { *pump = nullptr; });  // the closure is still running: it is freed from the event loop
        if (m_aborted)
            return end(false, tr("Aborted."));
        if (state->failed)
            return end(false, tr("Could not check the host: %1").arg(m_failure));
        const UploadPlan plan = planUpload(job.manifest, state->present, job.blobs);
        if (!plan)
            return end(false, plan.error);
        emit planned(plan);
        if (dryRun)
            return end(true, {});
        const QString token = job.token;
        const HostConfig host = m_host;
        runPlan(
            plan,
            [host, token](const Put& put) {
                Op op;
                op.kind = Op::Kind::Put;
                op.key = host.keyFor(token, put.key);
                op.size = put.size;
                if (put.kind == Put::Kind::Blob) {
                    op.file = put.file;
                    op.sha256 = put.sha256;
                    op.sha512 = put.sha512;
                } else {
                    op.body = put.body;
                }
                return op;
            },
            job.baseline, true, token, [this] { end(true, {}); });
    };
    (*pump)();
}

void Publisher::runPlan(const UploadPlan& plan, std::function<Op(const Put&)> toOp, std::optional<int> baseline, bool recheck, QString token,
                        std::function<void()> whenDone)
{
    m_sequencer = std::make_unique<UploadSequencer>(plan, m_maxConcurrentPuts);
    m_toOp = std::move(toOp);
    m_planDone = std::move(whenDone);
    m_baseline = baseline;
    m_recheck = recheck;
    m_token = std::move(token);
    m_sent.clear();
    m_total = plan.blobBytes;
    emit status(tr("Uploading..."));
    emit progress(0, m_total);
    pumpPlan();
}

void Publisher::pumpPlan()
{
    for (const int index : m_sequencer->next())
        startPut(index);
    if (!m_sequencer->finished())
        return;
    if (m_sequencer->succeeded()) {
        const auto whenDone = m_planDone;
        whenDone();
    } else {
        end(false, m_aborted ? tr("Aborted.") : m_failure);
    }
}

void Publisher::startPut(int index)
{
    QPointer<Publisher> self(this);
    const auto send = [this, self, index] {
        if (!self)
            return;
        if (m_aborted) {
            // abortAll() only stops what is in flight. A folder copy has nothing in flight between two files, so without this check the
            // publish would carry on to the end after Cancel. Failing the put keeps the order guarantee: no revision, no latest.json.
            OpResult result;
            result.error = QStringLiteral("aborted");
            return putDone(index, result);
        }
        const Put put = m_sequencer->plan().puts.at(index);
        startOp(m_toOp(put), 0, [this, self, index](const OpResult& result) {
            if (self)
                putDone(index, result);
        }, [this, self, index, kind = put.kind](qint64 current, qint64) {
            if (!self || kind != Put::Kind::Blob)
                return;
            m_sent.insert(index, current);
            qint64 sum = 0;
            for (const qint64 value : std::as_const(m_sent))
                sum += value;
            emit progress(std::min(sum, m_total), m_total);
        });
    };

    if (m_sequencer->plan().puts.at(index).kind != Put::Kind::Latest || !m_recheck) {
        send();
        return;
    }
    // Last look before the pack goes live: if latest.json moved since this publish began, somebody else published. The store cannot make
    // this atomic (no conditional write), so this only narrows the window; see the report.
    readLatest(m_token, [this, self, index, send](bool ok, std::optional<Latest> latest, const QString& error) {
        if (!self)
            return;
        if (!ok) {
            OpResult result;
            result.error = error;
            return putDone(index, result);
        }
        std::optional<int> now;
        if (latest)
            now = latest->revision;
        if (!latestUnchanged(m_baseline, now)) {
            OpResult result;
            result.status = 409;
            result.error = tr("Another owner published while this was running. Nothing was switched over; check the host and publish again.");
            return putDone(index, result);
        }
        send();
    });
}

void Publisher::putDone(int index, const OpResult& result)
{
    const Put put = m_sequencer->plan().puts.at(index);
    if (!result.ok) {
        if (m_failure.isEmpty())
            m_failure = tr("Could not send %1: %2").arg(what(put.kind), result.error);
        m_sequencer->done(index, false);
    } else {
        m_sent.insert(index, put.size);
        if (put.kind == Put::Kind::Blob) {
            qint64 sum = 0;
            for (const qint64 value : std::as_const(m_sent))
                sum += value;
            emit progress(std::min(sum, m_total), m_total);
        }
        m_sequencer->done(index, true);
    }
    pumpPlan();
}

// ---------------------------------------------------------------- rotate

void Publisher::rotate(const Manifest& manifest, const QString& oldToken, const QString& newToken)
{
    if (m_busy) {
        emit finished(false, tr("Another operation is still running."));
        return;
    }
    m_busy = true;
    m_aborted = false;
    m_failure.clear();

    const RotatePlan rotatePlan = planRotate(manifest);
    QHash<QString, qint64> sizes;
    for (const File& file : manifest.files)
        sizes.insert(blobKey(file.sha512), file.size);

    UploadPlan plan;
    for (const QString& key : rotatePlan.copyKeys) {
        Put put;
        put.kind = kindOfKey(key);
        put.key = key;
        put.size = sizes.value(key, 0);
        plan.blobBytes += put.size;
        plan.puts.append(put);
    }
    const HostConfig host = m_host;
    runPlan(
        plan,
        [host, oldToken, newToken](const Put& put) {
            Op op;
            op.kind = Op::Kind::Copy;
            op.key = host.keyFor(newToken, put.key);
            op.copyFrom = host.keyFor(oldToken, put.key);
            return op;
        },
        std::nullopt, false, newToken, [this, keys = rotatePlan.deleteKeys, oldToken] { runDeletes(keys, oldToken); });
}

void Publisher::runDeletes(QStringList keys, const QString& oldToken)
{
    emit status(tr("Removing the old folder..."));
    struct DeleteState {
        QStringList pending;
        int inFlight = 0;
        int failed = 0;
        bool done = false;
    };
    auto state = std::make_shared<DeleteState>();
    state->pending = std::move(keys);
    auto pump = std::make_shared<std::function<void()>>();
    QPointer<Publisher> self(this);
    *pump = [this, self, state, pump, oldToken] {
        if (!self || state->done)
            return;
        // latest.json (the first key) goes alone: from then on the old link answers "gone" and the rest is only housekeeping.
        const bool first = state->pending.size() > 0 && state->pending.first() == latestKey();
        while (!m_aborted && !state->pending.isEmpty() && state->inFlight < (first ? 1 : MaxConcurrentHeads)) {
            const QString key = state->pending.takeFirst();
            ++state->inFlight;
            Op op;
            op.kind = Op::Kind::Delete;
            op.key = m_host.keyFor(oldToken, key);
            startOp(op, 0, [this, self, state, pump](const OpResult& result) {
                if (!self)
                    return;
                --state->inFlight;
                if (!result.ok)
                    ++state->failed;
                (*pump)();
            }, {});
            if (first)
                break;
        }
        if (state->inFlight > 0 || (!m_aborted && !state->pending.isEmpty()))
            return;
        state->done = true;
        QTimer::singleShot(0, [pump] { *pump = nullptr; });
        const int failed = state->failed;
        // The new link works either way: leftovers in the old folder are a warning, not a failure.
        end(true, failed > 0 || m_aborted ? tr("The new link works, but %1 file(s) of the old folder could not be removed. Remove them on the host.").arg(failed) : QString());
    };
    (*pump)();
}

}  // namespace SharedPack
