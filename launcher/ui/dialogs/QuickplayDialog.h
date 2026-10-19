// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QDateTime>
#include <QDialog>
#include <QHash>
#include <QList>
#include <QMap>
#include <QSet>

#include "meta/VersionList.h"
#include "quickplay/QuickplayPreset.h"

class QCheckBox;
class QComboBox;

/// Lets the user pick a Minecraft version and a preset for the shared Quickplay instance.
class QuickplayDialog : public QDialog {
    Q_OBJECT
   public:
    /// The version lists of Fabric and of the loaders of the presets (Quickplay::loaderLists) must be loaded. packVersions has the
    /// Minecraft versions of each modpack preset (by preset ID); a modpack preset only offers those.
    QuickplayDialog(QList<Quickplay::Preset> presets,
                    Meta::VersionList::Ptr minecraft,
                    QHash<QString, QSet<QString>> packVersions = {},
                    QWidget* parent = nullptr);

    QString minecraftVersion() const;
    Quickplay::Preset preset() const;
    /// True when the chosen version is the newest release, so it should be stored as "follow the latest release".
    bool isLatestRelease() const;

    /// Asks for a version and preset, prepares the Quickplay instance (switching version, loader and mods) and launches it.
    static void run(QWidget* parent);

   private:
    void populateVersions();
    /// The Minecraft versions a loader supports, read once.
    const QSet<QString>& supported(Quickplay::Loader loader);

    QList<Quickplay::Preset> m_presets;
    QHash<QString, QSet<QString>> m_packVersions;
    Meta::VersionList::Ptr m_minecraft;
    QMap<Quickplay::Loader, QSet<QString>> m_supported;  // cache of supported()
    QDateTime m_oldest;                                  // release time of the oldest version Fabric supports
    QComboBox* m_version;
    QCheckBox* m_snapshots;
    QComboBox* m_preset;
    QString m_latestRelease;
};
