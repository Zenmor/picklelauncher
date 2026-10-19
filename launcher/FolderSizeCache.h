// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

/// Folder sizes for file trees (QFileSystemModel only knows the size of files). One background walk of a root folder adds up every folder
/// below it; the answers are cached and a view reads them without ever computing anything. No widgets: QtCore only.
namespace FolderSizes {

/// Size of every folder at or below a root, keyed by its path relative to the root with forward slashes ("" is the root itself). Links and
/// junctions are not followed and not counted. A folder is only recorded once all of it was read, so a stopped walk never leaves a wrong
/// (too small) number behind.
using Sizes = QHash<QString, qint64>;

/// Adds up \a root. Blocking: run it on a worker thread. \a cancel is polled between entries. \a onBatch, if set, gets the folders that
/// finished since the last call about every 150 ms and once more at the end; the return value is everything that finished.
Sizes compute(const QString& root, const std::atomic_bool& cancel, const std::function<void(const Sizes&)>& onBatch = {});

/// What a row of a tree shows. "..." while the walk still runs, nothing for a folder it could not count (a link), else the size in the
/// same format QFileSystemModel uses for files.
QString displayText(const std::optional<qint64>& size, bool walkFinished);

/// One row as the size column sees it.
struct SortKey {
    qint64 size = 0;
    bool known = true;  ///< false: not counted yet
    QString name;
};

/// Order for the size column among rows of the same kind (folders with folders, files with files). A folder that is not counted yet counts
/// as the biggest, so rows do not jump around among the known ones while sizes arrive. Equal sizes fall back to the name, always ascending
/// (QSortFilterProxyModel asks with swapped arguments for a descending sort, hence \a ascending). Strict weak ordering.
bool lessBySize(const SortKey& left, const SortKey& right, bool ascending);

}  // namespace FolderSizes

class FolderSizeCache : public QObject {
    Q_OBJECT
   public:
    explicit FolderSizeCache(QObject* parent = nullptr);
    /// Stops the walk and waits for the worker (it polls often, so this is short).
    ~FolderSizeCache() override;

    /// Forgets what it knew and starts walking \a root in the background.
    void start(const QString& root);
    /// Stops the walk. What was counted so far stays readable; late results of the stopped walk are ignored.
    void cancel();

    /// The size of the folder at \a relativePath (relative to the root given to start(), "" or "." is the root), if it is counted yet.
    std::optional<qint64> size(const QString& relativePath) const;
    /// The walk ran to the end. False while it runs and after cancel().
    bool finished() const { return m_finished; }

   signals:
    /// New sizes arrived (or the walk finished). Always on the thread of this object.
    void changed();

   private:
    void accept(int generation, const FolderSizes::Sizes& batch, bool last);

    QThreadPool m_pool;
    FolderSizes::Sizes m_sizes;
    std::shared_ptr<std::atomic_bool> m_cancel;
    int m_generation = 0;
    bool m_finished = false;
};
