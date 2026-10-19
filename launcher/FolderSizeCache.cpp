// SPDX-License-Identifier: GPL-3.0-only

#include "FolderSizeCache.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocale>
#include <QMetaObject>
#include <QRunnable>

#include "StringUtils.h"

namespace FolderSizes {

namespace {

struct Walker {
    const std::atomic_bool& cancel;
    const std::function<void(const Sizes&)>& onBatch;
    Sizes all;
    Sizes pending;
    QElapsedTimer sinceFlush;

    void flush()
    {
        if (onBatch && !pending.isEmpty())
            onBatch(pending);
        pending.clear();
        sinceFlush.restart();
    }

    // Everything below \a absoluteDir, or less when stopped (then nothing is recorded for it or the folders above it).
    qint64 walk(const QString& absoluteDir, const QString& relative)
    {
        qint64 total = 0;
        // NoSymLinks: a link or junction could point anywhere (or loop), and the pack scan never follows one either.
        const QFileInfoList entries =
            QDir(absoluteDir).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System | QDir::NoSymLinks,
                                            QDir::NoSort);
        for (const QFileInfo& entry : entries) {
            if (cancel.load())
                return total;
            if (entry.isDir()) {
                const QString name = entry.fileName();
                total += walk(entry.absoluteFilePath(), relative.isEmpty() ? name : relative + QLatin1Char('/') + name);
            } else {
                total += entry.size();
            }
        }
        if (cancel.load())
            return total;
        all.insert(relative, total);
        pending.insert(relative, total);
        if (sinceFlush.elapsed() >= 150)
            flush();
        return total;
    }
};

}  // namespace

Sizes compute(const QString& root, const std::atomic_bool& cancel, const std::function<void(const Sizes&)>& onBatch)
{
    Walker walker{ cancel, onBatch, {}, {}, {} };
    walker.sinceFlush.start();
    if (QFileInfo(root).isDir())
        walker.walk(QDir::cleanPath(root), QString());
    walker.flush();
    return walker.all;
}

QString displayText(const std::optional<qint64>& size, bool walkFinished)
{
    if (size)
        return QLocale::system().formattedDataSize(*size);
    return walkFinished ? QString() : QStringLiteral("...");
}

bool lessBySize(const SortKey& left, const SortKey& right, bool ascending)
{
    if (left.known && right.known && left.size != right.size)
        return left.size < right.size;
    if (left.known != right.known)
        return left.known;  // the one that is not counted yet is the bigger one
    const int byName = StringUtils::naturalCompare(left.name, right.name, Qt::CaseInsensitive);
    if (byName == 0)
        return false;
    return byName < 0 ? ascending : !ascending;
}

}  // namespace FolderSizes

FolderSizeCache::FolderSizeCache(QObject* parent) : QObject(parent)
{
    m_pool.setMaxThreadCount(1);  // one walk at a time: a file tree must not fight the launcher for the disk
}

FolderSizeCache::~FolderSizeCache()
{
    if (m_cancel)
        m_cancel->store(true);
    m_pool.waitForDone();
}

void FolderSizeCache::start(const QString& root)
{
    cancel();
    m_sizes.clear();
    m_finished = false;
    m_cancel = std::make_shared<std::atomic_bool>(false);
    const int generation = m_generation;  // cancel() above already moved it on: results of older walks no longer match
    FolderSizeCache* self = this;
    auto cancelFlag = m_cancel;
    m_pool.start(QRunnable::create([self, root, cancelFlag, generation] {
        const FolderSizes::Sizes all = FolderSizes::compute(root, *cancelFlag, [self, generation](const FolderSizes::Sizes& batch) {
            // Posted to the cache: if it is gone by then, Qt drops the call. It never touches the model from this thread.
            QMetaObject::invokeMethod(self, [self, generation, batch] { self->accept(generation, batch, false); }, Qt::QueuedConnection);
        });
        Q_UNUSED(all)
        if (!cancelFlag->load())
            QMetaObject::invokeMethod(self, [self, generation] { self->accept(generation, {}, true); }, Qt::QueuedConnection);
    }));
}

void FolderSizeCache::cancel()
{
    if (m_cancel)
        m_cancel->store(true);
    ++m_generation;
    m_finished = false;
}

std::optional<qint64> FolderSizeCache::size(const QString& relativePath) const
{
    const QString key = relativePath == QLatin1String(".") ? QString() : relativePath;
    const auto it = m_sizes.constFind(key);
    if (it == m_sizes.cend())
        return std::nullopt;
    return it.value();
}

void FolderSizeCache::accept(int generation, const FolderSizes::Sizes& batch, bool last)
{
    if (generation != m_generation)
        return;
    for (auto it = batch.cbegin(); it != batch.cend(); ++it)
        m_sizes.insert(it.key(), it.value());
    if (last)
        m_finished = true;
    emit changed();
}
