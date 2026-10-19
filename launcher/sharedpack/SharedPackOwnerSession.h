// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <atomic>
#include <memory>
#include <optional>

#include "QObjectPtr.h"
#include "SharedPackCredentials.h"
#include "SharedPackInvite.h"
#include "SharedPackLink.h"
#include "SharedPackPublisher.h"
#include "SharedPackScan.h"
#include "tasks/Task.h"

class MinecraftInstance;
class QThread;

namespace SharedPack {

class S3Target;
class UrlChecker;

/// What the Publish dialog shows before anything is uploaded.
struct Preview {
    Manifest manifest;
    UploadPlan plan;
    QStringList skipped;       ///< "path: why", left out on purpose
    QStringList warnings;      ///< things that cost something but did not stop it (Modrinth could not be asked)
    int publicUrlFiles = 0;    ///< files that members fetch from Modrinth
    int releasedCount = 0;     ///< unticked files and folders that exist here: members that have them keep them
    int previousRevision = 0;  ///< 0 on a first publish
    bool firstPublish = false;
};

/// The owner's side for ONE instance, step by step, for the Publish dialog. Everything with a rule is in the tested pure code; this
/// class only carries the steps out in order: scan and hash the game folder, ask Modrinth for the public address of each mod, check that
/// the addresses still answer, read latest.json to number the revision, build the manifest, plan the upload (prepare). Then upload and,
/// only when that succeeded, record the instance as the owner (publish). No widgets.
/// The instance must not be running: its files change under the hash.
class OwnerSession : public QObject {
    Q_OBJECT
   public:
    explicit OwnerSession(QString instanceId, QObject* parent = nullptr);
    ~OwnerSession() override;

    QString instanceName() const;
    /// The instance's folder and its game folder (empty when the instance is gone): where the dialog reads and saves the file selection.
    QString instanceRoot() const;
    QString gameRoot() const;
    /// The instance follows somebody else's pack (a link and no owner record naming it): it cannot publish.
    bool isMember() const { return m_isMember; }
    /// A pack this launcher owns through this instance: the host is fixed, only Publish and Rotate Link make sense.
    bool isOwner() const { return m_record.has_value(); }
    const std::optional<OwnerRecord>& record() const { return m_record; }
    /// The invite of the pack as it is now (owner only).
    std::optional<Invite> invite() const { return m_link ? std::optional<Invite>(m_link->invite) : std::nullopt; }

    /// Steps up to the dry run; emits prepared() or failed(). \a host is ignored for a pack that already has an owner record.
    /// \a unticked: files and folders (relative to the game folder) the owner does not publish. They are not read, not hashed and not in the
    /// manifest; the ones that exist are listed in it as released, so members keep their copy.
    void prepare(const HostConfig& host, const QString& packName, const QString& changelog, const QStringList& unticked = {});
    /// Uploads what prepare() planned and records the instance as the owner. Emits published() or failed().
    void publish();
    /// Moves the pack to a new token (the old link stops working). Emits rotated() or failed().
    void rotate();
    void abort();
    bool busy() const { return m_busy; }

   signals:
    void status(const QString& text);
    void progress(qint64 done, qint64 total);
    void prepared(const SharedPack::Preview& preview);
    /// \a invite is the string to give to the members.
    void published(int revision, const QString& invite);
    void rotated(const QString& invite, const QString& warning);
    void failed(const QString& reason);

   private:
    enum class Mode { Idle, Prepare, Publish, Rotate };

    MinecraftInstance* instance() const;
    void load();
    bool begin(Mode mode);
    void fail(const QString& reason);
    void makePublisher(const HostConfig& host);
    void startScan();
    void onScanDone();
    void resolveUrls();
    void onUrls(const QHash<QString, QString>& urls);
    void onChecked(const QSet<QString>& resolved);
    void readHost();
    void onLatest(bool ok, const std::optional<Latest>& latest, const QString& error);
    void buildJob(std::optional<int> hostRevision);
    void onFinished(bool ok, const QString& error);
    void finishPublish();
    void rotateWithManifest(const QByteArray& manifestJson);

    QString m_instanceId;
    Mode m_mode = Mode::Idle;
    bool m_busy = false;
    bool m_isMember = false;
    std::optional<Link> m_link;
    std::optional<OwnerRecord> m_record;

    // The run.
    HostConfig m_host;
    QString m_name;
    QString m_changelog;
    QStringList m_unticked;  ///< the owner's choice for this run (see prepare)
    std::optional<Invite> m_invite;  ///< the pack's invite: existing, or the new one of a first publish
    std::unique_ptr<PublishTarget> m_target;
    std::unique_ptr<Publisher> m_publisher;
    std::unique_ptr<UrlChecker> m_checker;
    Task::Ptr m_task;
    QThread* m_thread = nullptr;
    std::atomic_bool m_cancel{ false };
    std::shared_ptr<ScanResult> m_scan;
    QHash<QString, QString> m_urls;  ///< sha512 -> public address that resolves
    QStringList m_warnings;
    std::optional<PublishJob> m_job;
    Preview m_preview;
    UploadPlan m_lastPlan;
    Invite m_rotatedInvite;
};

}  // namespace SharedPack
