// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackDisk.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <filesystem>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace SharedPack {

namespace {

std::filesystem::path toPath(const QString& text)
{
    return std::filesystem::path(text.toStdWString());
}

}  // namespace

QString hashFile(const QString& absolutePath)
{
    QFile file(absolutePath);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(QCryptographicHash::Sha512);
    if (!hash.addData(&file) || file.error() != QFileDevice::NoError)
        return {};
    return QString::fromLatin1(hash.result().toHex());
}

bool isLinkOrReparsePoint(const QString& absolutePath)
{
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(toPath(absolutePath), ec);
    if (!ec && std::filesystem::is_symlink(status))
        return true;
#ifdef Q_OS_WIN
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(absolutePath.utf16()));
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return true;
#endif
    return false;
}

bool ancestorsAreSafe(const QString& gameDir, const QString& relativePath)
{
    const QStringList parts = relativePath.split(QLatin1Char('/'));
    QString current = QDir::cleanPath(gameDir);
    for (int i = 0; i + 1 < parts.size(); ++i) {
        current += QLatin1Char('/') + parts[i];
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(toPath(current), ec);
        if (std::filesystem::status_known(status) && status.type() == std::filesystem::file_type::not_found)
            return true;  // nothing below a missing folder can be a link
        if (ec || isLinkOrReparsePoint(current) || !std::filesystem::is_directory(status))
            return false;
    }
    return true;
}

DiskState scanDisk(const QString& gameDir, const QStringList& relativePaths)
{
    DiskState state;
    for (const QString& path : relativePaths) {
        const QString key = path.toLower();
        if (state.contains(key))
            continue;

        DiskEntry entry;
        if (!ancestorsAreSafe(gameDir, path)) {
            entry.kind = DiskKind::Unsafe;
            state.insert(key, entry);
            continue;
        }

        const QString absolute = QDir::cleanPath(gameDir) + QLatin1Char('/') + path;
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(toPath(absolute), ec);
        if (std::filesystem::status_known(status) && status.type() == std::filesystem::file_type::not_found)
            continue;  // missing: not in the map

        entry.kind = DiskKind::Unsafe;
        if (!ec && std::filesystem::is_regular_file(status) && !isLinkOrReparsePoint(absolute)) {
            const QString hash = hashFile(absolute);
            if (!hash.isEmpty()) {
                entry.kind = DiskKind::File;
                entry.sha512 = hash;
            }
        }
        state.insert(key, entry);
    }
    return state;
}

}  // namespace SharedPack
