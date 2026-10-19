// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackOwner.h"

#include <QCoreApplication>
#include <QDir>
#include <QRandomGenerator>
#include <cstring>
#include <vector>

#include "SharedPackManifest.h"
#include "SharedPackRedact.h"

namespace SharedPack {

namespace {

QString tr(const char* text)
{
    return QCoreApplication::translate("SharedPack::Owner", text);
}

bool isKeyLike(const QString& key)
{
    if (key.size() < 8 || key.size() > 128)
        return false;
    for (QChar c : key)
        if (c.unicode() < 0x21 || c.unicode() > 0x7e)
            return false;
    return true;
}

/// The read side of \a config for one pack, normalized and validated by the invite parser itself (endpoint, bucket, prefix, region,
/// keys): there is one set of rules, the one members apply.
std::optional<Invite> inviteFor(const HostConfig& config, const QString& packId, const QString& name, const QString& token)
{
    Invite invite;
    invite.packId = packId;
    invite.name = name;
    invite.prefix = config.kind == HostConfig::Kind::Relay ? QString() : config.prefix;
    invite.token = token;
    if (config.kind == HostConfig::Kind::S3) {
        invite.endpoint = config.endpoint;
        invite.bucket = config.bucket;
        invite.region = config.region.isEmpty() ? QStringLiteral("auto") : config.region;
        invite.accessKeyId = config.readKeyId;
        invite.secretAccessKey = config.readSecret;
    } else if (config.kind == HostConfig::Kind::Relay) {
        // Relay reads go through the Worker so Rotate Link can revoke old read tokens immediately.
        invite.version = 2;
        QString base = config.endpoint;
        while (base.endsWith(QLatin1Char('/')))
            base.chop(1);
        invite.endpoints.append(base + QStringLiteral("/read"));
        invite.endpoint = invite.endpoints.first();
    } else {
        invite.endpoint = config.endpoint;
    }
    const auto parsed = parseInvite(inviteToString(invite));
    if (!parsed)
        return std::nullopt;
    return *parsed.value;
}

}  // namespace

QString HostConfig::keyFor(const QString& token, const QString& relative) const
{
    QString key;
    if (!prefix.isEmpty())
        key = prefix + QLatin1Char('/');
    return key + token + QLatin1Char('/') + relative;
}

QStringList HostConfig::secrets() const
{
    QStringList result;
    for (const QString& secret : { writeKeyId, writeSecret, readKeyId, readSecret, publisherToken, packSecret })
        if (!secret.isEmpty())
            result.append(secret);
    return result;
}

QString validateHostConfig(const HostConfig& config)
{
    if (config.kind == HostConfig::Kind::Folder) {
        if (config.folder.isEmpty() || !QDir::isAbsolutePath(config.folder))
            return tr("Enter an absolute folder path, for example C:/packs.");
        if (!config.writeKeyId.isEmpty() || !config.readKeyId.isEmpty())
            return tr("A folder host does not use keys. Leave the key fields empty.");
    } else if (config.kind == HostConfig::Kind::Relay) {
        // The Worker controls publishing; static public URLs serve members and must be separate.
        const QUrl worker(config.endpoint, QUrl::StrictMode);
        if (!worker.isValid() || worker.scheme() != QLatin1String("https") || worker.host().isEmpty() ||
            !worker.userInfo().isEmpty() || (worker.path() != QStringLiteral("/") && !worker.path().isEmpty()) ||
            worker.hasQuery() || worker.hasFragment())
            return tr("Enter the Shared Packs Server address (it must start with https://).");
        if (!isKeyLike(config.publisherToken))
            return tr("Enter the publisher token from the server.");
    } else {
        // S3
        if (config.bucket.isEmpty())
            return tr("Enter the name of the bucket.");
        if (!isKeyLike(config.writeKeyId) || !isKeyLike(config.writeSecret))
            return tr("Enter the Access Key ID and the Secret Access Key of the key that can write.");
        if (!isKeyLike(config.readKeyId) || !isKeyLike(config.readSecret))
            return tr("Enter the Read-Only Key ID and the Read-Only Secret for your friends.");
        if (config.readKeyId == config.writeKeyId || config.readSecret == config.writeSecret)
            return tr("Use a separate read-only key for your friends. They must not get the key that can write.");
    }
    if (!inviteFor(config, QStringLiteral("check"), QString(), QStringLiteral("aaaaaaaaaaaaaaaaaaaaaa")))
        return config.kind == HostConfig::Kind::S3
                   ? tr("Check the Endpoint (it must start with https://), the Bucket, the Prefix and the Region.")
                   : config.kind == HostConfig::Kind::Relay
                   ? tr("Check the Server Address (it must start with https://).")
                   : tr("Check the Public Address (it must start with https://) and the Prefix.");
    return {};
}

HostConfig::Kind defaultHostKind(const std::optional<HostConfig::Kind>& ownerKind)
{
    return ownerKind.value_or(HostConfig::Kind::Relay);
}

HostRows hostRows(HostConfig::Kind kind, bool isOwner, bool tokenSaved, bool wantsNewCode)
{
    HostRows rows;
    const bool relay = kind == HostConfig::Kind::Relay;
    rows.otherHostPicker = !relay;
    rows.folder = kind == HostConfig::Kind::Folder;
    rows.s3 = kind == HostConfig::Kind::S3;
    // An owner never needs a code: the pack already has its server and its access.
    rows.publisherCode = relay && !isOwner && (!tokenSaved || wantsNewCode);
    rows.newCodeButton = relay && !isOwner && tokenSaved && !wantsNewCode;
    return rows;
}

QByteArray systemRandomBytes(int count)
{
    std::vector<quint32> words(static_cast<size_t>(count + 3) / 4);
    QRandomGenerator::system()->fillRange(words.data(), static_cast<qsizetype>(words.size()));
    QByteArray bytes(count, Qt::Uninitialized);
    std::memcpy(bytes.data(), words.data(), static_cast<size_t>(count));
    return bytes;
}

QString randomId(const RandomBytes& random)
{
    const QByteArray bytes = random(16);
    if (bytes.size() != 16)
        return {};
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

std::optional<Invite> newPackInvite(const HostConfig& config, const QString& name, const RandomBytes& random, QString* error)
{
    const auto fail = [error](const QString& why) {
        if (error)
            *error = why;
        return std::optional<Invite>();
    };

    if (const QString problem = validateHostConfig(config); !problem.isEmpty())
        return fail(problem);
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > Limits::MaxNameChars)
        return fail(QStringLiteral("the pack needs a name of 1 to %1 characters").arg(Limits::MaxNameChars));

    const QString packId = randomId(random);
    const QString token = randomId(random);
    if (packId.isEmpty() || token.isEmpty() || packId == token || !isValidPackId(packId) || !isValidToken(token))
        return fail(QStringLiteral("no random numbers available"));

    const auto invite = inviteFor(config, packId, trimmed, token);
    if (!invite)
        return fail(QStringLiteral("the invite could not be made"));
    return invite;
}

std::optional<Invite> rotatedInvite(const Invite& old, const RandomBytes& random)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        const QString token = randomId(random);
        if (token.isEmpty() || token == old.token || !isValidToken(token))
            continue;
        Invite invite = old;
        invite.token = token;
        return invite;
    }
    return std::nullopt;
}

Standing standingOf(const QString& instanceId, const std::optional<QString>& ownerInstanceId, bool hasLink)
{
    if (ownerInstanceId && !instanceId.isEmpty() && *ownerInstanceId == instanceId)
        return Standing::Owner;
    // "owner" in a state file proves nothing: it is copied with the instance. Without the credentials record naming this very instance
    // it is a member (when there is a link to follow) or nothing of ours.
    return hasLink ? Standing::Member : Standing::Unmanaged;
}

std::optional<State> reconcileState(const State& state, Standing standing)
{
    if (standing == Standing::Unmanaged)
        return std::nullopt;  // not ours: nothing is written to it
    const Role wanted = standing == Standing::Owner ? Role::Owner : Role::Member;
    if (state.role == wanted)
        return std::nullopt;
    State fixed = state;
    fixed.role = wanted;
    return fixed;
}

void registerSecrets(const HostConfig& config)
{
    for (const QString& secret : config.secrets())
        registerLogSecret(secret);
}

}  // namespace SharedPack
