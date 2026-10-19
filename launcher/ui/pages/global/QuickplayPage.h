// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QByteArray>
#include <QList>
#include <QPixmap>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QWidget>

#include "net/NetJob.h"
#include "quickplay/QuickplayPreset.h"
#include "tasks/Task.h"
#include "ui/pages/BasePage.h"

class QAction;
class QCheckBox;
class QComboBox;
class QFormLayout;
class QLineEdit;
class QPushButton;
class QStandardItemModel;
class ModListView;

/// Settings page of Quickplay: whether its instance is hidden and the presets with their mods. The presets are edited in memory
/// and written by apply().
class QuickplayPage : public QWidget, public BasePage {
    Q_OBJECT

   public:
    explicit QuickplayPage(QWidget* parent = nullptr);

    QString displayName() const override { return tr("Quickplay"); }
    QIcon icon() const override { return QIcon::fromTheme("launch"); }
    QString id() const override { return "quickplay-settings"; }
    bool apply() override;

   private:
    void refreshPresets(int select);
    void showPreset(int row);
    void addPreset();
    void addModpackPreset();
    void duplicatePreset();
    void removePreset();

    // The mod list shows the mods of the selected preset. It is rebuilt from the preset after every change, so the preset is the only
    // place that holds the order.
    void showMods(int select = -1);
    void updateModButtons();
    void addMod();
    void replaceMod();
    void removeMods();
    void moveMod(int offset);
    void viewHomepage();
    // What the list shows: the mods of a mods preset, the pack of a modpack preset.
    static QList<Quickplay::Mod> shownMods(const Quickplay::Preset& preset);
    QList<int> selectedMods() const;  // rows, ascending
    int currentMod() const;           // the row when exactly one is selected, else -1

    // Icons come from the icon URL stored with the mod. Presets from before there were icons get them from Modrinth by project ID.
    QPixmap pixmapOf(const Quickplay::Mod& mod) const;
    void refreshIcons();
    void loadIcons();
    void fetchProjects(const QStringList& ids);

    QList<Quickplay::Preset> m_presets;
    QByteArray m_savedJson;  // what is on disk, apply() only writes if the presets differ

    QSet<QString> m_requestedProjects;
    QSet<QUrl> m_requestedIcons;
    QList<Task::Ptr> m_projectTasks;  // running requests are kept alive here
    QList<NetJob::Ptr> m_iconJobs;

    QCheckBox* m_hideInstance;
    QComboBox* m_preset;
    QPushButton* m_removePreset;
    QFormLayout* m_form;
    QLineEdit* m_name;
    QComboBox* m_loader;
    QWidget* m_modsPanel;
    ModListView* m_mods;
    QStandardItemModel* m_modsModel;
    QAction* m_downloadMods;
    QAction* m_replaceMod;
    QAction* m_removeMod;
    QAction* m_moveUp;
    QAction* m_moveDown;
    QAction* m_viewHomepage;
};
