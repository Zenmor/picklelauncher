// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackCredentials.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUrl>

#ifdef Q_OS_WIN
#include <windows.h>
#include <aclapi.h>
#include <string>
#endif

#include "SharedPackManifest.h"
#include "SharedPackRedact.h"

namespace SharedPack {

namespace {

#ifdef Q_OS_WIN
// The data folder passes its inherited ACL on to the file, and on a normal install that lets every local account read the write keys.
// A protected DACL with one entry (the current user) cuts the inheritance and the other accounts.
bool restrictToCurrentUser(const QString& path)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    QByteArray buffer(static_cast<qsizetype>(size), 0);
    const bool haveUser = size > 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
    CloseHandle(token);
    if (!haveUser)
        return false;

    EXPLICIT_ACCESS_W access{};
    access.grfAccessPermissions = FILE_ALL_ACCESS;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid);
    PACL acl = nullptr;
    if (SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS)
        return false;

    std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    const DWORD result = SetNamedSecurityInfoW(native.data(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                               nullptr, nullptr, acl, nullptr);
    LocalFree(acl);
    return result == ERROR_SUCCESS;
}
#else
bool restrictToCurrentUser(const QString&)
{
    return true;  // POSIX modes were set on the file already
}
#endif

constexpr qint64 MaxFileBytes = 4 * 1024 * 1024;
constexpr int MaxPacks = 1000;
constexpr int MaxPublisherTokens = 100;

bool isWorkerUrl(const QString& text)
{
    const QUrl url(text, QUrl::StrictMode);
    return url.isValid() && url.scheme() == QLatin1String("https") && !url.host().isEmpty() && url.userInfo().isEmpty() &&
           !url.hasQuery() && !url.hasFragment() && text.size() <= 512;
}

bool isValidPublisherToken(const QString& token)
{
    if (token.size() < 8 || token.size() > 128)
        return false;
    for (QChar c : token)
        if (c.unicode() < 0x21 || c.unicode() > 0x7e)
            return false;
    return true;
}

bool writeCredentialsFile(const QString& dataDir, const Credentials& credentials)
{
    if (!QDir().mkpath(QDir::cleanPath(dataDir)))
        return false;
    QSaveFile file(credentialsPath(dataDir));
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(credentialsToJson(credentials)) < 0 || !file.commit())
        return false;
    if (!restrictToCurrentUser(credentialsPath(dataDir)))
        qWarning() << "SharedPack: could not restrict the access rights of the credentials file";
    return true;
}

QJsonObject toJson(const OwnerRecord& record)
{
    const HostConfig& host = record.host;
    QString kindStr;
    if (host.kind == HostConfig::Kind::S3)
        kindStr = QStringLiteral("s3");
    else if (host.kind == HostConfig::Kind::Relay)
        kindStr = QStringLiteral("relay");
    else
        kindStr = QStringLiteral("folder");
    QJsonObject object{ { QStringLiteral("instanceId"), record.instanceId },
                        { QStringLiteral("kind"), kindStr } };
    const auto put = [&object](const char* key, const QString& value) {
        if (!value.isEmpty())
            object.insert(QLatin1String(key), value);
    };
    put("folder", host.folder);
    put("endpoint", host.endpoint);
    put("bucket", host.bucket);
    put("prefix", host.prefix);
    put("region", host.region);
    put("writeKeyId", host.writeKeyId);
    put("writeSecret", host.writeSecret);
    put("readKeyId", host.readKeyId);
    put("readSecret", host.readSecret);
    // Relay-only fields
    put("publisherToken", host.publisherToken);
    put("packSecret", host.packSecret);
    put("readBaseUrl", host.readBaseUrl);
    put("fallbackReadUrl", host.fallbackReadUrl);
    return object;
}

std::optional<OwnerRecord> recordFromJson(const QJsonObject& object)
{
    bool ok = true;
    const auto text = [&object, &ok](const char* key) {
        const QJsonValue value = object.value(QLatin1String(key));
        if (value.isUndefined() || value.isNull())
            return QString();
        if (!value.isString())
            ok = false;
        return value.toString();
    };

    OwnerRecord record;
    record.instanceId = text("instanceId");
    const QString kind = text("kind");
    if (kind == QLatin1String("s3"))
        record.host.kind = HostConfig::Kind::S3;
    else if (kind == QLatin1String("folder"))
        record.host.kind = HostConfig::Kind::Folder;
    else if (kind == QLatin1String("relay"))
        record.host.kind = HostConfig::Kind::Relay;
    else
        return std::nullopt;
    record.host.folder = text("folder");
    record.host.endpoint = text("endpoint");
    record.host.bucket = text("bucket");
    record.host.prefix = text("prefix");
    record.host.region = text("region");
    record.host.writeKeyId = text("writeKeyId");
    record.host.writeSecret = text("writeSecret");
    record.host.readKeyId = text("readKeyId");
    record.host.readSecret = text("readSecret");
    // Relay-only fields
    record.host.publisherToken = text("publisherToken");
    record.host.packSecret = text("packSecret");
    record.host.readBaseUrl = text("readBaseUrl");
    record.host.fallbackReadUrl = text("fallbackReadUrl");

    if (!ok || record.instanceId.isEmpty() || record.instanceId.size() > 256 || !validateHostConfig(record.host).isEmpty())
        return std::nullopt;
    return record;
}

}  // namespace

QString credentialsPath(const QString& dataDir)
{
    return QDir::cleanPath(dataDir) + QStringLiteral("/sharedpack-credentials.json");
}

QByteArray credentialsToJson(const Credentials& credentials)
{
    QJsonObject packs;
    for (auto it = credentials.packs.cbegin(); it != credentials.packs.cend(); ++it)
        packs.insert(it.key(), toJson(it.value()));
    QJsonObject publishers;
    for (auto it = credentials.publisherTokens.cbegin(); it != credentials.publisherTokens.cend(); ++it)
        publishers.insert(it.key(), it.value());
    QJsonObject root{ { QStringLiteral("formatVersion"), FormatVersion },
                      { QStringLiteral("packs"), packs },
                      { QStringLiteral("publishers"), publishers } };
    if (!credentials.lastPublisherWorker.isEmpty())
        root.insert(QStringLiteral("lastPublisherWorker"), credentials.lastPublisherWorker);
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

std::optional<Credentials> parseCredentials(const QByteArray& json)
{
    if (json.size() > MaxFileBytes)
        return std::nullopt;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return std::nullopt;
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("formatVersion")).toInt(0) != FormatVersion || !root.value(QStringLiteral("packs")).isObject())
        return std::nullopt;

    const QJsonObject packs = root.value(QStringLiteral("packs")).toObject();
    if (packs.size() > MaxPacks)
        return std::nullopt;
    Credentials credentials;
    for (auto it = packs.begin(); it != packs.end(); ++it) {
        if (!isValidPackId(it.key()) || !it.value().isObject())
            return std::nullopt;
        const auto record = recordFromJson(it.value().toObject());
        if (!record)
            return std::nullopt;
        credentials.packs.insert(it.key(), *record);
    }
    const QJsonValue publisherValue = root.value(QStringLiteral("publishers"));
    if (!publisherValue.isUndefined()) {
        if (!publisherValue.isObject() || publisherValue.toObject().size() > MaxPublisherTokens)
            return std::nullopt;
        const QJsonObject publishers = publisherValue.toObject();
        for (auto it = publishers.begin(); it != publishers.end(); ++it) {
            if (!isWorkerUrl(it.key()) || !it.value().isString() || !isValidPublisherToken(it.value().toString()))
                return std::nullopt;
            credentials.publisherTokens.insert(it.key(), it.value().toString());
        }
    }
    const QJsonValue lastWorkerValue = root.value(QStringLiteral("lastPublisherWorker"));
    if (!lastWorkerValue.isUndefined()) {
        // A pointer without a token is not an error: the file still holds the owner records. findSavedPublisherWorker() checks the token.
        if (!lastWorkerValue.isString() || !isWorkerUrl(lastWorkerValue.toString()))
            return std::nullopt;
        credentials.lastPublisherWorker = lastWorkerValue.toString();
    }
    return credentials;
}

std::optional<Credentials> readCredentials(const QString& dataDir)
{
    QFile file(credentialsPath(dataDir));
    if (!file.exists())
        return Credentials{};
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    return parseCredentials(file.read(MaxFileBytes + 1));
}

bool writeOwnerRecord(const QString& dataDir, const QString& packId, const OwnerRecord& record)
{
    if (!isValidPackId(packId) || !validateHostConfig(record.host).isEmpty() || record.instanceId.isEmpty())
        return false;
    auto credentials = readCredentials(dataDir);
    if (!credentials)
        return false;  // damaged: never replaced, the keys of the other packs are in there
    credentials->packs.insert(packId, record);

    return writeCredentialsFile(dataDir, *credentials);
}

std::optional<OwnerRecord> findOwnerRecord(const QString& dataDir, const QString& packId)
{
    const auto credentials = readCredentials(dataDir);
    if (!credentials || !credentials->packs.contains(packId))
        return std::nullopt;
    const OwnerRecord record = credentials->packs.value(packId);
    registerSecrets(record.host);
    return record;
}

std::optional<QString> findPublisherToken(const QString& dataDir, const QString& workerUrl)
{
    const auto credentials = readCredentials(dataDir);
    if (!credentials || !credentials->publisherTokens.contains(workerUrl))
        return std::nullopt;
    const QString token = credentials->publisherTokens.value(workerUrl);
    registerLogSecret(token);
    return token;
}

std::optional<QString> findLastPublisherWorker(const QString& dataDir)
{
    const auto credentials = readCredentials(dataDir);
    if (!credentials || credentials->lastPublisherWorker.isEmpty())
        return std::nullopt;
    return credentials->lastPublisherWorker;
}

std::optional<QString> findSavedPublisherWorker(const QString& dataDir)
{
    const auto credentials = readCredentials(dataDir);
    if (!credentials || credentials->lastPublisherWorker.isEmpty() || !credentials->publisherTokens.contains(credentials->lastPublisherWorker))
        return std::nullopt;
    return credentials->lastPublisherWorker;
}

bool writePublisherToken(const QString& dataDir, const QString& workerUrl, const QString& token)
{
    if (!isWorkerUrl(workerUrl) || !isValidPublisherToken(token))
        return false;
    auto credentials = readCredentials(dataDir);
    if (!credentials || (!credentials->publisherTokens.contains(workerUrl) && credentials->publisherTokens.size() >= MaxPublisherTokens))
        return false;
    credentials->publisherTokens.insert(workerUrl, token);
    credentials->lastPublisherWorker = workerUrl;
    registerLogSecret(token);
    return writeCredentialsFile(dataDir, *credentials);
}

}  // namespace SharedPack
