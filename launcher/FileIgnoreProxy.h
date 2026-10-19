// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (C) 2022 Sefa Eyeoglu <contact@scrumplex.net>
 *  Copyright (C) 2023 TheKodeToad <TheKodeToad@proton.me>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * This file incorporates work covered by the following copyright and
 * permission notice:
 *
 *      Copyright 2013-2021 MultiMC Contributors
 *
 *      Licensed under the Apache License, Version 2.0 (the "License");
 *      you may not use this file except in compliance with the License.
 *      You may obtain a copy of the License at
 *
 *          http://www.apache.org/licenses/LICENSE-2.0
 *
 *      Unless required by applicable law or agreed to in writing, software
 *      distributed under the License is distributed on an "AS IS" BASIS,
 *      WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *      See the License for the specific language governing permissions and
 *      limitations under the License.
 */

#pragma once

#include <QColor>
#include <QFileInfo>
#include <QPointer>
#include <QSortFilterProxyModel>
#include <functional>
#include "SeparatorPrefixTree.h"

class FolderSizeCache;

class FileIgnoreProxy : public QSortFilterProxyModel {
    Q_OBJECT

   public:
    /// A colour and a tooltip for a row that deserves attention (see setMarker). An empty tooltip means no mark.
    struct Mark {
        QColor color;
        QString toolTip;
    };
    /// \a relativePath is relative to the root given to the constructor, \a size is 0 for folders, \a ticked is false when the row or a
    /// folder above it is unticked.
    using Marker = std::function<Mark(const QString& relativePath, bool isDir, qint64 size, bool ticked)>;

    FileIgnoreProxy(QString root, QObject* parent);
    // NOTE: Sadly, we have to do sorting ourselves.
    bool lessThan(const QModelIndex& left, const QModelIndex& right) const;

    virtual Qt::ItemFlags flags(const QModelIndex& index) const;

    virtual QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const;
    virtual bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole);

    QString relPath(const QString& path) const;

    bool setFilterState(QModelIndex index, Qt::CheckState state);

    bool shouldExpand(QModelIndex index);

    void setBlockedPaths(QStringList paths);

    /// Opt-in, off by default (the export dialogs behave as before): the size column shows the size of folders from \a cache ("..." until
    /// it is counted) and sorts folders by it. The cache is only ever READ here, never started or waited for: whoever enables this starts
    /// it for the same root as this proxy, and cancels it when the tree goes away. nullptr turns it off again.
    void setFolderSizes(FolderSizeCache* cache);

    /// Opt-in, off by default: \a marker is asked about every row the view draws, so it must be cheap and must not touch the disk.
    void setMarker(Marker marker);

    inline const SeparatorPrefixTree<'/'>& blockedPaths() const { return m_blocked; }
    inline SeparatorPrefixTree<'/'>& blockedPaths() { return m_blocked; }

    // list of file names that need to be removed completely from model
    inline QStringList& ignoreFilesWithName() { return m_ignoreFiles; }
    inline QStringList& ignoreFilesWithSuffix() { return m_ignoreFilesSuffixes; }
    // list of relative paths that need to be removed completely from model
    inline SeparatorPrefixTree<'/'>& ignoreFilesWithPath() { return m_ignoreFilePaths; }

    bool filterFile(const QFileInfo& fileName) const;

    void loadBlockedPathsFromFile(const QString& fileName);

    void saveBlockedPathsToFile(const QString& fileName);

   protected:
    bool filterAcceptsColumn(int source_column, const QModelIndex& source_parent) const;
    bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const;

    bool ignoreFile(QFileInfo file) const;

   private:
    void folderSizesChanged();

    const QString m_root;
    SeparatorPrefixTree<'/'> m_blocked;
    QStringList m_ignoreFiles;
    QStringList m_ignoreFilesSuffixes;
    SeparatorPrefixTree<'/'> m_ignoreFilePaths;
    QPointer<FolderSizeCache> m_folderSizes;
    Marker m_marker;
};
