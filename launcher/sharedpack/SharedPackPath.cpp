// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackPath.h"

#include <QSet>
#include <QStringList>
#include <algorithm>

namespace SharedPack {

namespace {

constexpr int MaxPathChars = 240;
constexpr int MaxSegmentChars = 100;

bool fail(QString* why, const char* reason)
{
    if (why)
        *why = QString::fromLatin1(reason);
    return false;
}

bool isForbiddenChar(QChar c)
{
    return c.unicode() < 0x20 || c.unicode() == 0x7f || QStringLiteral("<>:\"|?*\\").contains(c);
}

// Windows refuses these names, with or without an extension ("con.txt" is CON).
bool isReservedDeviceName(const QString& segment)
{
    static const QSet<QString> devices{ "CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$" };
    const QString stem = segment.section('.', 0, 0).trimmed().toUpper();
    if (devices.contains(stem))
        return true;
    // COM1 to COM9 and LPT1 to LPT9, which Windows also reads with the superscript digits 1, 2 and 3 (U+00B9, U+00B2, U+00B3).
    auto isPortDigit = [](QChar c) { return c.isDigit() || c.unicode() == 0xb9 || c.unicode() == 0xb2 || c.unicode() == 0xb3; };
    return stem.size() == 4 && (stem.startsWith("COM") || stem.startsWith("LPT")) && isPortDigit(stem[3]);
}

}  // namespace

bool isSafePath(const QString& path, QString* why)
{
    if (path.isEmpty())
        return fail(why, "empty path");
    if (path.size() > MaxPathChars)
        return fail(why, "path too long");
    if (path.endsWith(QStringLiteral(".packnew"), Qt::CaseInsensitive))
        return fail(why, "path ends with .packnew, which the launcher uses itself");

    // Keep empty parts: they are what an absolute path ("/x", "//server/x") or a double slash turns into.
    for (const QString& segment : path.split('/')) {
        if (segment.isEmpty())
            return fail(why, "empty path segment (absolute path or double slash)");
        if (segment == QLatin1String(".") || segment == QLatin1String(".."))
            return fail(why, "'.' or '..' in path");
        if (segment.size() > MaxSegmentChars)
            return fail(why, "path segment too long");
        if (segment.endsWith('.') || segment.endsWith(' '))
            return fail(why, "path segment ends with a dot or a space");
        if (std::any_of(segment.cbegin(), segment.cend(), isForbiddenChar))
            return fail(why, "forbidden character in path (backslash, drive letter or stream, wildcard, control character)");
        if (isReservedDeviceName(segment))
            return fail(why, "reserved Windows device name in path");
    }
    return true;
}

FileClass classify(const QString& path)
{
    const QString lower = path.toLower();
    const QString top = lower.section('/', 0, 0);

    static const QSet<QString> excluded{ "saves", "screenshots", "logs", "crash-reports" };
    if (excluded.contains(top))
        return FileClass::Excluded;
    // Runtime libraries a mod downloads into the game folder by itself ("mcef-libraries", "rinku-libraries": the Chromium engine, hundreds
    // of MB). Every member's own mod fetches its copy, and the Shared Packs Server refuses files above 100 MB.
    if (top.endsWith(QLatin1String("-libraries")))
        return FileClass::Excluded;
    // The launcher's own mod metadata. It describes what the member installed, so a pack must neither carry nor overwrite it.
    if (lower.contains(QLatin1String("/.index/")))
        return FileClass::Excluded;
    if (lower == QLatin1String("options.txt") || lower == QLatin1String("servers.dat"))
        return FileClass::SeedOnly;

    static const QSet<QString> packWins{ "mods", "resourcepacks", "shaderpacks" };
    if (lower.contains('/') && packWins.contains(top))
        return FileClass::PackWins;
    return FileClass::Merge;
}

PathSet::PathSet(const QStringList& paths)
{
    for (QString path : paths) {
        while (path.endsWith(QLatin1Char('/')))
            path.chop(1);
        if (!path.isEmpty())
            m_lower.insert(path.toLower());
    }
}

bool PathSet::covers(const QString& path) const
{
    if (m_lower.isEmpty())
        return false;
    const QString lower = path.toLower();
    if (m_lower.contains(lower))
        return true;
    for (qsizetype slash = lower.indexOf(QLatin1Char('/')); slash != -1; slash = lower.indexOf(QLatin1Char('/'), slash + 1)) {
        if (m_lower.contains(lower.left(slash)))
            return true;
    }
    return false;
}

}  // namespace SharedPack
