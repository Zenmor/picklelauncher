// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackManifest.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <limits>

#include "SharedPackPath.h"

namespace SharedPack {

namespace {

const QString MinecraftUid = QStringLiteral("net.minecraft");

// The readers below throw this, the two parse functions catch it. Nothing leaves this file.
struct Invalid {
    QString what;
};

[[noreturn]] void invalid(const QString& what)
{
    throw Invalid{ what };
}

// Control characters, line and paragraph separators, and the bidirectional overrides that make text display as something it is not.
bool hasBadChars(const QString& text, bool allowLayout)
{
    return std::any_of(text.cbegin(), text.cend(), [allowLayout](QChar c) {
        if (allowLayout && (c == '\n' || c == '\t'))
            return false;
        const ushort u = c.unicode();
        return u < 0x20 || u == 0x7f || u == 0x2028 || u == 0x2029 || (u >= 0x202a && u <= 0x202e) || (u >= 0x2066 && u <= 0x2069);
    });
}

QString readString(const QJsonObject& object, const char* key, int minChars, int maxChars, bool allowLayout = false)
{
    const QJsonValue value = object.value(QLatin1String(key));
    if (!value.isString())
        invalid(QStringLiteral("\"%1\" is missing or not a string").arg(key));
    const QString text = value.toString();
    if (text.size() < minChars || text.size() > maxChars)
        invalid(QStringLiteral("\"%1\" has a bad length").arg(key));
    if (hasBadChars(text, allowLayout))
        invalid(QStringLiteral("\"%1\" has control characters").arg(key));
    return text;
}

qint64 readInt(const QJsonObject& object, const char* key, qint64 min, qint64 max)
{
    const QJsonValue value = object.value(QLatin1String(key));
    if (!value.isDouble())
        invalid(QStringLiteral("\"%1\" is missing or not a number").arg(key));
    const double number = value.toDouble();
    if (number != std::floor(number) || number < static_cast<double>(min) || number > static_cast<double>(max))
        invalid(QStringLiteral("\"%1\" is out of range").arg(key));
    return static_cast<qint64>(number);
}

QJsonArray readArray(const QJsonObject& object, const char* key, int maxItems)
{
    const QJsonValue value = object.value(QLatin1String(key));
    if (!value.isArray())
        invalid(QStringLiteral("\"%1\" is missing or not an array").arg(key));
    const QJsonArray array = value.toArray();
    if (array.size() > maxItems)
        invalid(QStringLiteral("\"%1\" has too many entries").arg(key));
    return array;
}

QJsonObject readObjectEntry(const QJsonValue& value, const char* what)
{
    if (!value.isObject())
        invalid(QStringLiteral("%1 entry is not an object").arg(what));
    return value.toObject();
}

bool isPlainText(const QString& text)
{
    return !text.isEmpty() && text.trimmed() == text && !hasBadChars(text, false);
}

QJsonObject parseObject(const QByteArray& json, qint64 maxBytes)
{
    if (json.size() > maxBytes)
        invalid(QStringLiteral("document too large"));
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        invalid(parseError.errorString());
    if (!document.isObject())
        invalid(QStringLiteral("document is not an object"));
    return document.object();
}

// Shared by both documents: size limit, JSON, formatVersion (a newer one is not read any further), then \a fill for the rest.
template <typename T, typename Fill>
Parsed<T> parseDocument(const QByteArray& json, qint64 maxBytes, Fill fill)
{
    Parsed<T> result;
    try {
        const QJsonObject object = parseObject(json, maxBytes);
        T value;
        value.formatVersion = static_cast<int>(readInt(object, "formatVersion", 1, std::numeric_limits<int>::max()));
        if (value.formatVersion > FormatVersion) {
            result.failure = Failure::TooNew;
            result.error = QStringLiteral("format version %1 is newer than the supported %2").arg(value.formatVersion).arg(FormatVersion);
            return result;
        }
        fill(object, value);
        result.value = std::move(value);
    } catch (const Invalid& e) {
        result.failure = Failure::Corrupt;
        result.error = e.what;
    }
    return result;
}

void readComponents(const QJsonObject& object, Manifest& manifest)
{
    static const QSet<QString> loaders{ "net.fabricmc.fabric-loader", "org.quiltmc.quilt-loader", "net.minecraftforge", "net.neoforged" };

    QSet<QString> seen;
    int loaderCount = 0;
    for (const QJsonValue& entry : readArray(object, "components", 8)) {
        const QJsonObject item = readObjectEntry(entry, "component");
        Component component{ readString(item, "uid", 1, 64), readString(item, "version", 1, 64) };
        if (!isPlainText(component.version))
            invalid(QStringLiteral("bad component version"));
        if (component.uid != MinecraftUid && !loaders.contains(component.uid))
            invalid(QStringLiteral("unsupported component"));
        if (loaders.contains(component.uid))
            ++loaderCount;
        if (seen.contains(component.uid))
            invalid(QStringLiteral("duplicate component"));
        seen.insert(component.uid);
        manifest.components.append(component);
    }
    if (!seen.contains(MinecraftUid))
        invalid(QStringLiteral("no net.minecraft component"));
    if (loaderCount > 1)
        invalid(QStringLiteral("more than one mod loader"));
}

void readFile(const QJsonObject& item, File& file)
{
    file.path = readString(item, "path", 1, 1024);
    QString why;
    if (!isSafePath(file.path, &why))
        invalid(why);
    if (classify(file.path) == FileClass::Excluded)
        invalid(QStringLiteral("a pack cannot contain saves, screenshots, logs or crash reports"));

    file.size = readInt(item, "size", 0, Limits::MaxFileSize);
    file.sha512 = readString(item, "sha512", 128, 128);
    if (!isValidSha512(file.sha512))
        invalid(QStringLiteral("\"sha512\" is not 128 lower-case hex digits"));

    if (item.contains(QLatin1String("urls"))) {
        for (const QJsonValue& url : readArray(item, "urls", Limits::MaxUrlsPerFile)) {
            if (!url.isString() || !isAllowedUrl(url.toString()))
                invalid(QStringLiteral("a file URL is not an acceptable https URL"));
            file.urls.append(url.toString());
        }
    }
}

void readFiles(const QJsonObject& object, Manifest& manifest)
{
    QSet<QString> lowerPaths;  // the file systems we run on ignore case
    qint64 total = 0;
    for (const QJsonValue& entry : readArray(object, "files", Limits::MaxFiles)) {
        File file;
        readFile(readObjectEntry(entry, "file"), file);
        if (lowerPaths.contains(file.path.toLower()))
            invalid(QStringLiteral("two files with the same path"));
        lowerPaths.insert(file.path.toLower());
        total += file.size;
        if (total > Limits::MaxTotalSize)
            invalid(QStringLiteral("the files add up to more than the allowed size"));
        manifest.files.append(file);
    }

    // A file cannot also be a folder: "config" and "config/a.toml" would fail halfway through an update.
    for (const QString& path : lowerPaths) {
        for (qsizetype slash = path.indexOf('/'); slash != -1; slash = path.indexOf('/', slash + 1)) {
            if (lowerPaths.contains(path.left(slash)))
                invalid(QStringLiteral("a path is both a file and a folder"));
        }
    }
}

// Optional. Every entry is a path a member's launcher compares against what it installed, so it gets the same check as a file path.
void readReleased(const QJsonObject& object, Manifest& manifest)
{
    if (!object.contains(QLatin1String("released")))
        return;
    QSet<QString> lowerPaths;
    for (const QJsonValue& entry : readArray(object, "released", Limits::MaxReleased)) {
        if (!entry.isString())
            invalid(QStringLiteral("a released path is not a string"));
        const QString path = entry.toString();
        QString why;
        if (!isSafePath(path, &why))
            invalid(QStringLiteral("a released path is not acceptable: %1").arg(why));
        if (lowerPaths.contains(path.toLower()))
            invalid(QStringLiteral("a released path is listed twice"));
        lowerPaths.insert(path.toLower());
        manifest.released.append(path);
    }
}

}  // namespace

qint64 Manifest::totalSize() const
{
    qint64 total = 0;
    for (const File& file : files)
        total += file.size;
    return total;
}

bool isValidSha512(const QString& hex)
{
    return hex.size() == 128 &&
           std::all_of(hex.cbegin(), hex.cend(), [](QChar c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

bool isValidPackId(const QString& id)
{
    return !id.isEmpty() && id.size() <= 64 && std::all_of(id.cbegin(), id.cend(), [](QChar c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}

bool isAllowedUrl(const QString& text)
{
    if (text.size() > Limits::MaxUrlChars)
        return false;
    const QUrl url(text, QUrl::StrictMode);
    if (!url.isValid() || url.scheme() != QLatin1String("https"))
        return false;
    if (!url.userInfo().isEmpty() || url.hasFragment() || (url.port() != -1 && url.port() != 443))
        return false;
    // The hash makes a wrong download harmless, but a hostile manifest could still point every member's launcher at an address of its
    // choosing. So the only public source is Modrinth's CDN; anything else is uploaded to the pack's own host. Widening this list is a
    // format change (FormatVersion), so older launchers say "too new" instead of "corrupt".
    return url.host(QUrl::FullyEncoded).toLower() == QLatin1String("cdn.modrinth.com") &&
           url.path(QUrl::FullyEncoded).startsWith(QLatin1String("/data/"));
}

Parsed<Manifest> parseManifest(const QByteArray& json)
{
    return parseDocument<Manifest>(json, Limits::MaxManifestBytes, [](const QJsonObject& object, Manifest& manifest) {
        manifest.packId = readString(object, "packId", 1, 64);
        if (!isValidPackId(manifest.packId))
            invalid(QStringLiteral("bad packId"));
        manifest.name = readString(object, "name", 1, Limits::MaxNameChars);
        if (!isPlainText(manifest.name))
            invalid(QStringLiteral("bad name"));
        manifest.revision = static_cast<int>(readInt(object, "revision", 1, Limits::MaxRevision));
        if (object.contains(QLatin1String("changelog")))
            manifest.changelog = readString(object, "changelog", 0, Limits::MaxChangelogChars, true);
        readComponents(object, manifest);
        readFiles(object, manifest);
        readReleased(object, manifest);
    });
}

Parsed<Latest> parseLatest(const QByteArray& json)
{
    return parseDocument<Latest>(json, Limits::MaxLatestBytes, [](const QJsonObject& object, Latest& latest) {
        latest.packId = readString(object, "packId", 1, 64);
        if (!isValidPackId(latest.packId))
            invalid(QStringLiteral("bad packId"));
        latest.revision = static_cast<int>(readInt(object, "revision", 1, Limits::MaxRevision));
    });
}

}  // namespace SharedPack
