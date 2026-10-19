// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QDialog>
#include <QList>

#include "PlaytimeBreakdown.h"

class QScrollArea;

/// Read-only table of where the playtime went (pickle-playtime): one row per modpack, per instance and for deleted instances.
/// A click on a modpack row or an instance row shows details about it in a pane under the table.
class PlaytimeDialog : public QDialog {
    Q_OBJECT
   public:
    explicit PlaytimeDialog(const PlaytimeBreakdown::Result& breakdown, QWidget* parent = nullptr);

    /// Shows the dialog for the current playtime of all instances and of the ledger of deleted ones.
    static void run(QWidget* parent);

   private:
    void showHint();
    void showDetails(int rowIndex);

    QList<PlaytimeBreakdown::Row> m_rows;
    QScrollArea* m_pane = nullptr;
};
