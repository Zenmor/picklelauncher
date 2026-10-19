// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>

#include "SharedPackInvite.h"
#include "SharedPackState.h"

namespace SharedPack {

/// Where an owner publishes to. The write keys are secrets of the owner's computer only: they never go into an invite, an instance folder
/// or a log (see SharedPackCredentials for where they live).
struct HostConfig {
    enum class Kind {
        Folder,  ///< a folder on this computer (served over HTTPS by something else); write side only
        S3,      ///< an S3-compatible bucket, path-style, SigV4 (Cloudflare R2: region "auto")
        Relay,   ///< Shared Packs Server (Cloudflare Worker relay)
    };

    Kind kind = Kind::S3;
    QString folder;        ///< Folder: where the pack folders are written
    QString endpoint;      ///< S3: https://<account>.r2.cloudflarestorage.com. Folder/Relay: server or public address
    QString bucket;        ///< S3
    QString prefix;        ///< optional folder below the bucket / the endpoint
    QString region;        ///< S3, "auto" when empty
    QString writeKeyId;    ///< S3, used by the owner only
    QString writeSecret;   ///< S3, used by the owner only
    QString readKeyId;     ///< S3, goes into the invite: should be a READ-ONLY key
    QString readSecret;    ///< S3, goes into the invite
    // Relay fields
    QString publisherToken;   ///< Relay: authentication token for Worker
    QString packSecret;       ///< Relay: secret for this specific pack
    QString readBaseUrl;      ///< Relay: public base URL for reads (e.g. https://packs.zenmor.xyz)
    QString fallbackReadUrl;  ///< Relay: fallback public base URL (e.g. https://pub-xxx.r2.dev)

    /// "<prefix>/<token>/<relative>" without a leading slash: the key of a document of a pack on the host, the path below the folder for a
    /// Folder target.
    QString keyFor(const QString& token, const QString& relative) const;

    /// Everything that must not reach a log.
    QStringList secrets() const;
};

/// Empty when the configuration is usable, otherwise what to fix: one translated sentence that names the field (no secrets in it). An S3
/// invite carries the read keys, so a write key given as the read key is refused: it would hand every member the right to overwrite the
/// pack.
QString validateHostConfig(const HostConfig& config);

/// The host a new pack starts with: the Shared Packs Server. An owner keeps the host of the pack that already exists.
HostConfig::Kind defaultHostKind(const std::optional<HostConfig::Kind>& ownerKind);

/// Which rows of the "Publish Shared Pack" form are shown. No widgets: the dialog only applies it.
struct HostRows {
    bool otherHostPicker = false;  ///< the Folder / S3 choice below "Other hosts (advanced)"
    bool folder = false;
    bool s3 = false;
    bool publisherCode = false;
    bool newCodeButton = false;  ///< "I have a new Publisher Code", for a saved server whose access no longer works
};
/// \a tokenSaved: this computer already holds a publisher token for the server. \a wantsNewCode: the person asked to enter another code.
HostRows hostRows(HostConfig::Kind kind, bool isOwner, bool tokenSaved, bool wantsNewCode);

using RandomBytes = std::function<QByteArray(int)>;
/// QRandomGenerator::system(): the operating system's generator.
QByteArray systemRandomBytes(int count);

/// 16 random bytes as base64url without padding: 22 characters, 128 bits, valid for isValidToken and isValidPackId.
QString randomId(const RandomBytes& random);

/// The invite of a new pack: fresh packId and token, the host's read side. Empty when the configuration or the random source is unusable
/// (\a error says why).
std::optional<Invite> newPackInvite(const HostConfig& config, const QString& name, const RandomBytes& random, QString* error = nullptr);

/// "Rotate Link": the same pack (packId stays: the playtime ledger and ManagedPackID key on it) under a new token. Empty when the random
/// source returned the old token again.
std::optional<Invite> rotatedInvite(const Invite& old, const RandomBytes& random);

/// What a launcher does with an instance that has pack files.
enum class Standing {
    Owner,      ///< published it from this launcher: never receives updates, may publish
    Member,     ///< follows the pack
    Unmanaged,  ///< not ours to touch
};

/// The owner role belongs to ONE instance of ONE launcher: the one the credentials record names. A copy of the owner's instance (same
/// state file, new instance id) or an instance imported on another computer has the "owner" word in its state but is not the owner: it is
/// a plain member when it has a link file to follow, otherwise unmanaged. \a ownerInstanceId is the instance the credentials file names
/// for this pack, empty when there is none.
Standing standingOf(const QString& instanceId, const std::optional<QString>& ownerInstanceId, bool hasLink);

/// The state to write back when \a standing says the file lies (an "owner" state on a member): the same state as a member. Empty when
/// there is nothing to fix.
std::optional<State> reconcileState(const State& state, Standing standing);

/// Registers every secret of \a config with the log redactor. Call before any request that carries them.
void registerSecrets(const HostConfig& config);

}  // namespace SharedPack
