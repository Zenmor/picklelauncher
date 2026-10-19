// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <optional>

namespace SharedPack {

/// The schema version this launcher writes and reads. Within a version only fields are ever added and readers ignore unknown ones; a
/// change that older readers could misread bumps the version, and they refuse the pack instead of guessing.
constexpr int FormatVersion = 1;

namespace Limits {
constexpr qint64 MaxManifestBytes = 8 * 1024 * 1024;
constexpr qint64 MaxLatestBytes = 4 * 1024;
constexpr int MaxFiles = 5000;
constexpr int MaxReleased = 1000;
constexpr qint64 MaxFileSize = 512LL * 1024 * 1024;
constexpr qint64 MaxTotalSize = 4LL * 1024 * 1024 * 1024;
constexpr int MaxUrlsPerFile = 4;
constexpr int MaxUrlChars = 2048;
constexpr int MaxNameChars = 64;
constexpr int MaxChangelogChars = 20000;
constexpr int MaxRevision = 2147483647;
}  // namespace Limits

struct Component {
    QString uid;      ///< net.minecraft or one mod loader
    QString version;  ///< as the launcher's metadata names it
};

struct File {
    QString path;      ///< relative to the game folder, validated by SharedPack::isSafePath
    qint64 size = 0;   ///< bytes, checked after download
    QString sha512;    ///< 128 lower-case hex digits, the name of the blob and the identity of the content
    QStringList urls;  ///< optional public sources (Modrinth CDN only, see isAllowedUrl), tried before the pack's own blob
};

struct Manifest {
    int formatVersion = FormatVersion;
    QString packId;
    QString name;
    int revision = 0;
    QString changelog;
    QList<Component> components;  ///< net.minecraft first, then the loader if any
    QList<File> files;
    /// Files and folders (relative paths, a folder covers everything below it) the owner has in the game folder but chose NOT to publish.
    /// A member's launcher leaves them alone: a path that an earlier revision carried and that is listed here is not deleted. A path the
    /// owner removed from the game folder is not listed, so it is still removed from the members (that is how a mod is dropped).
    /// Optional; readers that do not know it ignore it.
    QStringList released;

    qint64 totalSize() const;
};

/// latest.json: what a member's launcher fetches at start to learn whether there is anything new.
struct Latest {
    int formatVersion = FormatVersion;
    QString packId;
    int revision = 0;
};

enum class Failure {
    Corrupt,  ///< truncated, not JSON, wrong types, a limit or a rule broken
    TooNew,   ///< written by a newer launcher: tell the member to update the launcher
};

template <typename T>
struct Parsed {
    std::optional<T> value;
    Failure failure = Failure::Corrupt;  ///< meaningful only without a value
    QString error;                       ///< for the log, English

    explicit operator bool() const { return value.has_value(); }
};

/// Parses and fully validates a revision manifest. Nothing in a manifest is trusted: every field is typed, bounded and checked here, so the
/// rest of the code can rely on it.
Parsed<Manifest> parseManifest(const QByteArray& json);
Parsed<Latest> parseLatest(const QByteArray& json);

bool isValidSha512(const QString& hex);
bool isValidPackId(const QString& id);

/// An https URL we are willing to fetch from a manifest: Modrinth's CDN (cdn.modrinth.com/data/...), default port, no credentials or
/// fragment.
bool isAllowedUrl(const QString& url);

}  // namespace SharedPack
