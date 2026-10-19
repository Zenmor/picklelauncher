// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QList>
#include <QSet>
#include <functional>
#include <QString>

#include "QObjectPtr.h"
#include "SharedPackTarget.h"
#include "net/NetRequest.h"

namespace SharedPack {

class S3Operation;

/// An S3-compatible bucket (Cloudflare R2: region "auto"): PUT, HEAD, GET, DELETE and server-side copy with the owner's write keys,
/// through the launcher's Net layer (proxy settings, user agent, timeouts, log with the redactor). Needs a running Application.
/// The rules (what is signed, how the URL looks) are in buildS3Request, which is tested; this class only sends it.
class S3Target : public PublishTarget {
    Q_OBJECT
   public:
    explicit S3Target(HostConfig host, QObject* parent = nullptr);
    ~S3Target() override;

    void run(const Op& op, Done done, Progress progress = {}) override;
    void abortAll() override;

   private:
    HostConfig m_host;
    QList<shared_qobject_ptr<S3Operation>> m_active;  ///< finished ones are dropped from the event loop, never from their own signal
};

class HeadOperation;

/// Which of some public addresses (Modrinth CDN) still answer a HEAD with success: a mod keeps its address only in the manifest when it
/// does, otherwise it gets a blob of its own. Anything that does not answer in a clean 2xx (a redirect to another host, a host that does
/// not allow HEAD, a timeout) counts as "does not resolve": the safe direction, it only costs an upload.
class UrlChecker : public QObject {
    Q_OBJECT
   public:
    static constexpr int MaxConcurrent = 6;
    using Done = std::function<void(const QSet<QString>& resolved)>;

    explicit UrlChecker(QObject* parent = nullptr) : QObject(parent) {}
    ~UrlChecker() override;

    void check(const QStringList& urls, Done done);
    void abort();

   private:
    void pump();

    QStringList m_pending;
    QList<shared_qobject_ptr<HeadOperation>> m_active;
    QSet<QString> m_resolved;
    Done m_done;
    bool m_running = false;
    bool m_aborted = false;
};
}  // namespace SharedPack
