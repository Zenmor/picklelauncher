// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackApply.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <filesystem>

#include "SharedPackDisk.h"

namespace SharedPack {

namespace {

constexpr qint64 SpaceMargin = 64LL * 1024 * 1024;

QString tr(const char* text)
{
    return QCoreApplication::translate("SharedPack::Apply", text);
}

std::filesystem::path toPath(const QString& text)
{
    return std::filesystem::path(text.toStdWString());
}

QString absolute(const Layout& layout, const QString& relative)
{
    return QDir::cleanPath(layout.gameDir) + QLatin1Char('/') + relative;
}

/// Creates the folders above \a relative one at a time, refusing anything that is not a plain directory.
bool makeParents(const Layout& layout, const QString& relative)
{
    QStringList parts = relative.split(QLatin1Char('/'));
    parts.removeLast();
    QString current = QDir::cleanPath(layout.gameDir);
    for (const QString& part : parts) {
        current += QLatin1Char('/') + part;
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(toPath(current), ec);
        if (std::filesystem::status_known(status) && status.type() == std::filesystem::file_type::not_found) {
            if (!QDir().mkdir(current))
                return false;
            continue;
        }
        if (ec || isLinkOrReparsePoint(current) || !std::filesystem::is_directory(status))
            return false;
    }
    return true;
}

void pruneEmptyFolders(const Layout& layout, const QString& relative)
{
    QStringList parts = relative.split(QLatin1Char('/'));
    parts.removeLast();
    QDir game(QDir::cleanPath(layout.gameDir));
    while (!parts.isEmpty()) {
        const QString folder = parts.join(QLatin1Char('/'));
        if (isLinkOrReparsePoint(game.filePath(folder)) || !game.rmdir(folder))  // rmdir only removes an empty folder
            break;
        parts.removeLast();
    }
}

/// Deletes the launcher's own mod-manager metadata (<folder>/.index/*.pw.toml with filename = "<jar>") of a file we replace or remove.
void removeStaleIndex(const Layout& layout, const QString& relative, QStringList* removed)
{
    const qsizetype slash = relative.lastIndexOf(QLatin1Char('/'));
    const QString folder = slash < 0 ? QString() : relative.left(slash + 1);
    QString name = relative.mid(slash + 1);
    if (name.endsWith(QStringLiteral(".disabled"), Qt::CaseInsensitive))
        name.chop(9);

    const QDir indexDir(absolute(layout, folder + QStringLiteral(".index")));
    if (!indexDir.exists())
        return;
    static const QRegularExpression filenameLine(QStringLiteral(R"re(^\s*filename\s*=\s*"(.*)"\s*$)re"));
    for (const QString& entry : indexDir.entryList({ QStringLiteral("*.pw.toml") }, QDir::Files)) {
        QFile file(indexDir.filePath(entry));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        bool match = false;
        while (!file.atEnd() && !match) {
            const auto m = filenameLine.match(QString::fromUtf8(file.readLine()).trimmed());
            if (m.hasMatch()) {
                QString indexed = m.captured(1);
                if (indexed.endsWith(QStringLiteral(".disabled"), Qt::CaseInsensitive))
                    indexed.chop(9);
                match = indexed.compare(name, Qt::CaseInsensitive) == 0;
                break;  // one filename per metadata file
            }
        }
        file.close();
        if (match && QFile::remove(indexDir.filePath(entry)))
            removed->append(folder + QStringLiteral(".index/") + entry);
    }
}

/// Copies the blob to the staging file, re-checks it, gives it a new mtime and renames it over \a relative. Empty = ok.
QString installBlob(const Layout& layout, const QString& sha512, const QString& relative)
{
    const QString target = absolute(layout, relative);
    const QString blob = layout.blobPath(sha512);
    const QString stage = layout.workDir + QStringLiteral("/stage");

    if (!ancestorsAreSafe(layout.gameDir, relative) || !makeParents(layout, relative))
        return tr("Cannot write %1: a folder on its path is a link or not a folder").arg(relative);

    std::error_code ec;
    const auto existing = std::filesystem::symlink_status(toPath(target), ec);
    const bool exists = !(std::filesystem::status_known(existing) && existing.type() == std::filesystem::file_type::not_found);
    if (exists && (ec || !std::filesystem::is_regular_file(existing) || isLinkOrReparsePoint(target)))
        return tr("Cannot replace %1: it is not a regular file").arg(relative);

    QFile::remove(stage);
    if (!QFile::copy(blob, stage))
        return tr("Cannot copy the downloaded file for %1").arg(relative);
    if (hashFile(stage) != sha512) {
        QFile::remove(stage);
        return tr("The downloaded file for %1 changed on disk").arg(relative);
    }
    {
        QFile file(stage);
        if (file.open(QIODevice::ReadWrite)) {
            file.setFileTime(QDateTime::currentDateTime(), QFileDevice::FileModificationTime);
            file.close();
        }
    }
    std::filesystem::rename(toPath(stage), toPath(target), ec);
    if (ec) {
        QFile::remove(stage);
        return tr("Cannot write %1 (is the game running?): %2").arg(relative, QString::fromStdString(ec.message()));
    }
    return {};
}

QString removeTarget(const Layout& layout, const QString& relative)
{
    const QString target = absolute(layout, relative);
    if (!ancestorsAreSafe(layout.gameDir, relative))
        return tr("Cannot remove %1: a folder on its path is a link or not a folder").arg(relative);
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(toPath(target), ec);
    if (std::filesystem::status_known(status) && status.type() == std::filesystem::file_type::not_found)
        return {};  // already gone: a repeated apply
    if (ec || !std::filesystem::is_regular_file(status) || isLinkOrReparsePoint(target))
        return tr("Cannot remove %1: it is not a regular file").arg(relative);
    if (!std::filesystem::remove(toPath(target), ec) || ec)
        return tr("Cannot remove %1 (is the game running?)").arg(relative);
    return {};
}

}  // namespace

qint64 requiredFreeBytes(const Plan& plan, const Manifest& manifest)
{
    QHash<QString, qint64> sizeByHash;
    QHash<QString, qint64> sizeByPath;
    for (const File& file : manifest.files) {
        sizeByHash.insert(file.sha512, file.size);
        sizeByPath.insert(file.path, file.size);
    }
    qint64 total = SpaceMargin;
    for (const QString& blob : blobsNeeded(plan))
        total += sizeByHash.value(blob);
    for (const Action& action : plan.actions)
        if (action.kind == ActionKind::Write || action.kind == ActionKind::PackNew)
            total += sizeByPath.value(action.path);
    return total;
}

State stateAfter(const State& oldState, const Plan& plan)
{
    State state = oldState;
    state.revision = plan.revision;
    state.installed = plan.installed;
    return state;
}

ApplyResult applyPlan(const Layout& layout, const State& oldState, const Plan& plan)
{
    ApplyResult result;
    if (!plan) {
        result.error = tr("This update cannot be applied to this instance");
        return result;
    }
    if (!QDir().mkpath(layout.workDir)) {
        result.error = tr("Cannot create the working folder");
        return result;
    }

    for (const Action& action : plan.actions) {
        QString error;
        switch (action.kind) {
            case ActionKind::Write:
                error = installBlob(layout, action.sha512, action.path);
                if (error.isEmpty()) {
                    removeStaleIndex(layout, action.path, &result.indexRemoved);
                    result.written.append(action.path);
                }
                break;
            case ActionKind::PackNew:
                error = installBlob(layout, action.sha512, action.path + QStringLiteral(".packnew"));
                if (error.isEmpty())
                    result.packNew.append(action.path);
                break;
            case ActionKind::Delete:
                error = removeTarget(layout, action.path);
                if (error.isEmpty()) {
                    removeStaleIndex(layout, action.path, &result.indexRemoved);
                    pruneEmptyFolders(layout, action.path);
                    result.deleted.append(action.path);
                }
                break;
            case ActionKind::Skip:
                if (action.why == Why::UnsafeOnDisk || action.why == Why::BadPath)
                    result.skipped.append(action.path);
                break;
            case ActionKind::Keep:
                break;
        }
        if (!error.isEmpty()) {
            result.error = error;
            return result;
        }
    }

    QSaveFile stateFile(layout.stateFile);
    if (!stateFile.open(QIODevice::WriteOnly) || stateFile.write(stateToJson(stateAfter(oldState, plan))) < 0 || !stateFile.commit()) {
        result.error = tr("Cannot save the state of the shared pack");
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace SharedPack
