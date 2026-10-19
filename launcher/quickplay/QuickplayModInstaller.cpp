// SPDX-License-Identifier: GPL-3.0-only

#include "QuickplayModInstaller.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QUrl>
#include <algorithm>
#include <optional>

#include "Application.h"
#include "BuildConfig.h"
#include "FileSystem.h"
#include "Json.h"
#include "Version.h"
#include "modplatform/ResourceAPI.h"
#include "modplatform/flame/FlameAPI.h"
#include "modplatform/helpers/HashUtils.h"
#include "modplatform/modrinth/ModrinthAPI.h"
#include "net/ApiDownload.h"
#include "net/ChecksumValidator.h"

namespace Quickplay {

namespace {

// Dependency chains are short; this only stops a misbehaving API from looping forever.
constexpr int MaxResolveRounds = 20;

struct ManifestFile {
    QString name;
    QString hashType;  // "sha512" for Modrinth, "sha1" for CurseForge
    QString hash;
};

// File names come from the network and from a file on disk, never let them leave the mods folder.
bool isPlainFileName(const QString& name)
{
    return !name.isEmpty() && name != "." && name != ".." && QFileInfo(name).fileName() == name;
}

// The hashes we can check a download with, in the algorithms the providers give.
std::optional<QCryptographicHash::Algorithm> checksumAlgorithm(Hashing::Algorithm algorithm)
{
    switch (algorithm) {
        case Hashing::Algorithm::Sha512:
            return QCryptographicHash::Sha512;
        case Hashing::Algorithm::Sha1:
            return QCryptographicHash::Sha1;
        case Hashing::Algorithm::Md5:
            return QCryptographicHash::Md5;
        default:
            return std::nullopt;
    }
}

QList<ManifestFile> readManifest(const QString& path)
{
    QList<ManifestFile> files;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return files;
    for (const auto& value : QJsonDocument::fromJson(file.readAll()).object().value("files").toArray()) {
        const auto obj = value.toObject();
        ManifestFile entry{ obj.value("name").toString(), obj.value("hash_type").toString(), obj.value("hash").toString() };
        if (entry.hashType.isEmpty()) {  // version 1 only knew sha512
            entry.hashType = "sha512";
            entry.hash = obj.value("sha512").toString();
        }
        if (isPlainFileName(entry.name))
            files.append(entry);
    }
    return files;
}

bool writeManifest(const QString& path, const QList<ManifestFile>& files)
{
    QJsonArray array;
    for (const auto& f : files)
        array.append(QJsonObject{ { "name", f.name }, { "hash_type", f.hashType }, { "hash", f.hash } });

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(QJsonObject{ { "version", 2 }, { "files", array } }).toJson(QJsonDocument::Indented));
    return file.commit();
}

ResourceAPI& apiFor(ModPlatform::ResourceProvider provider)
{
    static ModrinthAPI modrinth;
    static FlameAPI flame;
    if (provider == ModPlatform::ResourceProvider::FLAME)
        return flame;
    return modrinth;
}

// The same request the "Download mods" dialog makes for the versions of a mod.
std::optional<QUrl> projectVersionsUrl(const ProjectRef& project, const QString& minecraftVersion, ModPlatform::ModLoaderTypes loaders)
{
    auto pack = std::make_shared<ModPlatform::IndexedPack>();
    pack->addonId = project.projectId;
    pack->provider = project.provider;

    const ResourceAPI::VersionSearchArgs args{ pack, std::vector<Version>{ Version(minecraftVersion) }, loaders,
                                               ModPlatform::ResourceType::Mod, false };
    if (const auto url = apiFor(project.provider).getVersionsURL(args))
        return QUrl(*url);
    return std::nullopt;
}

QString projectKey(const ProjectRef& project)
{
    return Mod{ project.projectId, {}, {}, project.provider }.key();
}

const ModPlatform::IndexedVersion* findVersion(const ProviderVersions& store,
                                               ModPlatform::ResourceProvider provider,
                                               const QString& projectId,
                                               const QString& versionId)
{
    const auto providerStore = store.constFind(provider);
    if (providerStore == store.constEnd())
        return nullptr;
    const auto it = providerStore->constFind(projectId);
    if (it == providerStore->constEnd())
        return nullptr;
    for (const auto& version : *it) {
        if (version.fileId.toString() == versionId)
            return &version;
    }
    return nullptr;
}

}  // namespace

ModInstallTask::ModInstallTask(QString minecraftVersion, Loader loader, QList<Mod> mods, QString modsDir, QString manifestPath)
    : m_minecraftVersion(std::move(minecraftVersion))
    , m_loaders(modLoaderTypes(loader, m_minecraftVersion))
    , m_mods(std::move(mods))
    , m_modsDir(std::move(modsDir))
    , m_manifestPath(std::move(manifestPath))
{}

QString ModInstallTask::displayName(const ProjectRef& project) const
{
    const auto it = std::ranges::find_if(
        m_mods, [&](const Mod& mod) { return mod.provider == project.provider && mod.projectId == project.projectId; });
    if (it != m_mods.end())
        return it->name;
    // A dependency of a mod, the user has not seen it in the preset
    return tr("%1 (%2)").arg(project.projectId, ModPlatform::ProviderCapabilities::readableName(project.provider));
}

QStringList ModInstallTask::warnings() const
{
    QStringList skipped;
    QStringList blocked;
    for (const auto& project : m_result.skippedProjects)
        (m_blocked.contains(projectKey(project)) ? blocked : skipped).append(displayName(project));

    QStringList result;
    if (!skipped.isEmpty())
        result.append(tr("No compatible version for Minecraft %1, skipped: %2").arg(m_minecraftVersion, skipped.join(", ")));
    if (!blocked.isEmpty())
        result.append(tr("The authors of these mods do not allow other programs to download them, skipped: %1").arg(blocked.join(", ")));
    return result + m_warnings;
}

void ModInstallTask::executeTask()
{
    setStatus(tr("Resolving mods..."));
    resolveStep();
}

void ModInstallTask::get(const QList<QUrl>& urls, Responses done)
{
    m_job.reset(new NetJob(tr("Quickplay mod data"), APPLICATION->network()));
    auto responses = std::make_shared<QList<QByteArray*>>();
    for (const auto& url : urls) {
        auto [action, response] = Net::ApiDownload::makeByteArray(url);
        m_job->addNetAction(action);
        responses->append(response);
    }
    connect(m_job.get(), &Task::failed, this, [this](QString reason) { emitFailed(tr("Failed to fetch mod data: %1").arg(reason)); });
    connect(m_job.get(), &Task::succeeded, this, [this, responses, done] {
        QList<QByteArray> data;
        for (const auto* response : *responses)
            data.append(*response);
        // The callback starts the next job, which must not replace this one while it is still emitting its signal.
        QMetaObject::invokeMethod(this, [done, data] { done(data); }, Qt::QueuedConnection);
    });
    m_job->start();
}

bool ModInstallTask::parseVersions(const ProjectRef& project, const QByteArray& data, bool isList)
{
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError)
        return false;

    // Modrinth answers with an array, CurseForge with an object that has the array in "data"
    QJsonArray array;
    if (isList) {
        if (!doc.isArray() && !doc.object().value("data").isArray())
            return false;
        array = doc.isArray() ? doc.array() : doc.object().value("data").toArray();
    } else {
        if (!doc.isObject())
            return false;
        array = QJsonArray{ doc.object() };
    }

    auto& api = apiFor(project.provider);
    auto& versions = m_store[project.provider][project.projectId];  // creates the entry, an empty list means "fetched, nothing compatible"
    int withoutDownload = 0;
    try {
        for (const auto& value : array) {
            auto obj = value.toObject();
            auto version = api.loadIndexedPackVersion(obj, ModPlatform::ResourceType::Mod);
            if (!version.fileId.isValid())
                continue;
            if (!version.addonId.isValid())
                version.addonId = project.projectId;
            // CurseForge can only filter by one loader, and not at all for several (Quilt also takes Fabric mods)
            if (project.provider == ModPlatform::ResourceProvider::FLAME && !(version.loaders & m_loaders))
                continue;
            if (version.downloadUrl.isEmpty()) {  // the author does not allow downloads by other programs
                withoutDownload++;
                continue;
            }
            versions.append(version);
        }
    } catch (const Json::JsonException&) {
        return false;
    }

    if (isList) {
        // Newest first. Modrinth already sorts like that, CurseForge does not promise it.
        std::ranges::stable_sort(versions, [](const auto& a, const auto& b) { return a.date > b.date; });

        // Prefer stable releases. A project that only has alpha/beta builds for this version keeps them instead of being dropped.
        const auto isRelease = [](const ModPlatform::IndexedVersion& v) {
            return v.version_type == ModPlatform::IndexedVersionType::Release;
        };
        if (std::ranges::any_of(versions, isRelease))
            versions.removeIf([&](const auto& v) { return !isRelease(v); });

        if (versions.isEmpty() && withoutDownload > 0)
            m_blocked.insert(projectKey(project));
    }
    return true;
}

void ModInstallTask::resolveStep()
{
    QList<ProjectRef> wanted;
    for (const auto& mod : m_mods)
        wanted.append({ mod.provider, mod.projectId });
    m_result = resolve(wanted, m_store);

    switch (m_result.status) {
        case ResolveResult::Status::Failed:
            emitFailed(m_result.error);
            return;
        case ResolveResult::Status::Done:
            fetchPinnedVersions();
            return;
        case ResolveResult::Status::NeedVersions:
            break;
    }

    if (++m_rounds > MaxResolveRounds) {
        emitFailed(tr("The mod dependencies are nested too deeply."));
        return;
    }

    const QList<ProjectRef> missing = m_result.missingProjects;
    QList<QUrl> urls;
    for (const auto& project : missing) {
        const auto url = projectVersionsUrl(project, m_minecraftVersion, m_loaders);
        if (!url) {
            emitFailed(tr("Could not build a request for the mod %1.").arg(displayName(project)));
            return;
        }
        urls.append(*url);
    }

    get(urls, [this, missing](const QList<QByteArray>& data) {
        for (int i = 0; i < missing.size(); i++) {
            if (!parseVersions(missing[i], data[i], true)) {
                emitFailed(tr("%1 returned invalid data for the mod %2.")
                               .arg(ModPlatform::ProviderCapabilities::readableName(missing[i].provider), displayName(missing[i])));
                return;
            }
        }
        resolveStep();
    });
}

// A required dependency can pin an exact version that was not part of the fetched lists, ask for those by id.
// Only Modrinth has such dependencies, CurseForge names the project and nothing else.
void ModInstallTask::fetchPinnedVersions()
{
    QList<ResolvedMod> missing;
    QList<QUrl> urls;
    for (const auto& mod : m_result.mods) {
        if (findVersion(m_store, mod.provider, mod.projectId, mod.versionId))
            continue;
        if (mod.provider != ModPlatform::ResourceProvider::MODRINTH) {
            emitFailed(tr("Could not load version %1 of the mod %2.").arg(mod.versionId, displayName({ mod.provider, mod.projectId })));
            return;
        }
        missing.append(mod);
        urls.append(QUrl(QString("%1/version/%2").arg(BuildConfig.MODRINTH_PROD_URL, mod.versionId)));
    }

    if (missing.isEmpty()) {
        install();
        return;
    }

    get(urls, [this, missing](const QList<QByteArray>& data) {
        for (int i = 0; i < missing.size(); i++) {
            const ProjectRef project{ missing[i].provider, missing[i].projectId };
            if (!parseVersions(project, data[i], false) ||
                !findVersion(m_store, project.provider, project.projectId, missing[i].versionId)) {
                emitFailed(tr("Could not load version %1 of the mod %2.").arg(missing[i].versionId, displayName(project)));
                return;
            }
        }
        install();
    });
}

void ModInstallTask::install()
{
    struct Download {
        QUrl url;
        QString path;
        ManifestFile file;
        Hashing::Algorithm hashAlgorithm;
        QCryptographicHash::Algorithm checksumAlgorithm;
    };

    QList<Download> downloads;
    QHash<QString, QString> fileOwners;  // lower case file name -> the mod that has it

    for (const auto& mod : m_result.mods) {
        const ProjectRef project{ mod.provider, mod.projectId };
        const auto* version = findVersion(m_store, mod.provider, mod.projectId, mod.versionId);
        if (!version)
            continue;  // cannot happen, fetchPinnedVersions() made sure of it

        const QString name = displayName(project);
        const auto algorithm = Hashing::algorithmFromString(version->hash_type);
        const auto checksum = checksumAlgorithm(algorithm);
        if (!isPlainFileName(version->fileName) || !checksum || version->hash.isEmpty()) {
            m_warnings.append(tr("%1 was skipped, its file cannot be checked.").arg(name));
            continue;
        }

        // The same mod from both providers has the same file name. Fabric would refuse to start with it twice.
        const auto owner = fileOwners.constFind(version->fileName.toLower());
        if (owner != fileOwners.constEnd()) {
            m_warnings.append(tr("%1 was skipped, %2 already installs the file %3.").arg(name, *owner, version->fileName));
            continue;
        }
        fileOwners.insert(version->fileName.toLower(), name);

        downloads.append({ QUrl(version->downloadUrl),
                           QDir(m_modsDir).filePath(version->fileName),
                           { version->fileName, version->hash_type, version->hash.toLower() },
                           algorithm,
                           *checksum });
    }

    if (!FS::ensureFolderPathExists(m_modsDir)) {
        emitFailed(tr("Could not create the mods folder %1.").arg(m_modsDir));
        return;
    }

    // Remove what an earlier run installed and the new set no longer contains.
    for (const auto& old : readManifest(m_manifestPath)) {
        const bool keep = std::ranges::any_of(downloads, [&](const auto& d) {
            return d.file.name == old.name && d.file.hashType == old.hashType && d.file.hash == old.hash;
        });
        if (!keep)
            QFile::remove(QDir(m_modsDir).filePath(old.name));
    }

    // Recorded before downloading, so a download that fails half way is still cleaned up by the next run.
    QList<ManifestFile> manifest;
    for (const auto& download : downloads)
        manifest.append(download.file);
    if (!writeManifest(m_manifestPath, manifest)) {
        emitFailed(tr("Could not write %1.").arg(m_manifestPath));
        return;
    }

    m_job.reset(new NetJob(tr("Quickplay mods"), APPLICATION->network()));
    bool anyDownload = false;
    for (const auto& download : downloads) {
        const bool upToDate = QFileInfo::exists(download.path) &&
                              Hashing::hash(download.path, download.hashAlgorithm).compare(download.file.hash, Qt::CaseInsensitive) == 0;
        if (upToDate)
            continue;
        auto action = Net::ApiDownload::makeFile(download.url, download.path);
        action->addValidator(new Net::ChecksumValidator(download.checksumAlgorithm, QByteArray::fromHex(download.file.hash.toLatin1())));
        m_job->addNetAction(action);
        anyDownload = true;
    }

    if (!anyDownload) {
        emitSucceeded();
        return;
    }

    setStatus(tr("Downloading mods..."));
    connect(m_job.get(), &Task::progress, this, &Task::setProgress);
    connect(m_job.get(), &Task::failed, this, [this](QString reason) { emitFailed(tr("Failed to download mods: %1").arg(reason)); });
    connect(m_job.get(), &Task::succeeded, this, [this] { emitSucceeded(); });
    m_job->start();
}

}  // namespace Quickplay
