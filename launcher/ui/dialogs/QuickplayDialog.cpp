// SPDX-License-Identifier: GPL-3.0-only

#include "QuickplayDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QPushButton>
#include <QVBoxLayout>
#include <algorithm>
#include <memory>

#include "Application.h"
#include "InstanceList.h"
#include "meta/Index.h"
#include "quickplay/QuickplayPackTask.h"
#include "quickplay/QuickplayPrepareTask.h"
#include "tasks/ConcurrentTask.h"
#include "ui/dialogs/CustomMessageBox.h"
#include "ui/dialogs/ProgressDialog.h"

namespace {

const QString MinecraftUid = QStringLiteral("net.minecraft");
const QString VersionSetting = QStringLiteral("QuickplayVersion");
const QString PresetSetting = QStringLiteral("QuickplayPreset");

void showError(QWidget* parent, const QString& text)
{
    CustomMessageBox::selectable(parent, QObject::tr("Error"), text, QMessageBox::Critical)->exec();
}

// Runs the task in a modal progress dialog. Returns true if it succeeded; failures are reported to the user.
bool runModal(QWidget* parent, Task* task)
{
    struct State {
        bool succeeded = false;
        QString error;
    };
    auto state = std::make_shared<State>();
    QObject::connect(task, &Task::succeeded, task, [state] { state->succeeded = true; });
    QObject::connect(task, &Task::failed, task, [state](QString reason) { state->error = std::move(reason); });

    ProgressDialog dialog(parent);
    dialog.setSkipButton(true, QObject::tr("Abort"));
    dialog.execWithTask(task);

    if (!state->error.isEmpty())
        showError(parent, state->error);
    return state->succeeded;
}

}  // namespace

QuickplayDialog::QuickplayDialog(QList<Quickplay::Preset> presets,
                                 Meta::VersionList::Ptr minecraft,
                                 QHash<QString, QSet<QString>> packVersions,
                                 QWidget* parent)
    : QDialog(parent), m_presets(std::move(presets)), m_packVersions(std::move(packVersions)), m_minecraft(std::move(minecraft))
{
    setWindowTitle(tr("Quickplay"));

    // Quickplay is made for Fabric, so no preset goes further back than the oldest version Fabric supports.
    const auto fabric = supported(Quickplay::Loader::Fabric);
    for (const auto& version : m_minecraft->versions()) {
        if (fabric.contains(version->version()) && (!m_oldest.isValid() || version->time() < m_oldest))
            m_oldest = version->time();
    }

    m_preset = new QComboBox(this);
    for (const auto& preset : m_presets) {
        const QString details = preset.kind == Quickplay::Kind::Modpack ? tr("modpack")
                                : preset.loader == Quickplay::Loader::None
                                    ? tr("no mods")
                                    : tr("%1, %n mod(s)", nullptr, preset.mods.size()).arg(Quickplay::loaderName(preset.loader));
        m_preset->addItem(QString("%1 (%2)").arg(preset.name, details), preset.id);
    }

    m_version = new QComboBox(this);
    m_snapshots = new QCheckBox(tr("Show snapshots"), this);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Play"));

    auto* form = new QFormLayout();
    form->addRow(tr("Preset"), m_preset);
    form->addRow(tr("Minecraft version"), m_version);
    form->addRow(QString(), m_snapshots);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(buttons);

    // Restore the last choice. A stored snapshot needs the snapshot filter on to be selectable.
    auto* settings = APPLICATION->settings();
    m_preset->setCurrentIndex(qMax(0, m_preset->findData(settings->get(PresetSetting).toString())));
    if (const auto stored = m_minecraft->getVersion(settings->get(VersionSetting).toString()); stored && stored->type() == "snapshot")
        m_snapshots->setChecked(true);
    populateVersions();
    if (const int index = m_version->findData(settings->get(VersionSetting).toString()); index >= 0)
        m_version->setCurrentIndex(index);

    // A preset with a mod loader only offers the versions that loader supports, so the list depends on the preset.
    connect(m_preset, &QComboBox::currentIndexChanged, this, &QuickplayDialog::populateVersions);
    connect(m_snapshots, &QCheckBox::toggled, this, &QuickplayDialog::populateVersions);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_version, &QComboBox::currentIndexChanged, this,
            [buttons, this] { buttons->button(QDialogButtonBox::Ok)->setEnabled(m_version->currentIndex() >= 0); });
    buttons->button(QDialogButtonBox::Ok)->setEnabled(m_version->currentIndex() >= 0);
}

void QuickplayDialog::populateVersions()
{
    const QString previous = m_version->currentData().toString();
    const Quickplay::Loader loader = preset().loader;
    const bool isPack = preset().kind == Quickplay::Kind::Modpack;  // only the versions the pack has files for

    m_version->clear();
    m_latestRelease.clear();
    int latestIndex = -1;
    for (const auto& version : m_minecraft->versions()) {  // newest first
        const bool release = version->type() == "release";
        if (!release && !(m_snapshots->isChecked() && version->type() == "snapshot"))
            continue;
        // Plain Minecraft goes as far back as Fabric does. A mod loader only offers what it supports.
        const bool allowed = isPack                              ? m_packVersions.value(preset().id).contains(version->version())
                             : loader == Quickplay::Loader::None ? !m_oldest.isValid() || version->time() >= m_oldest
                                                                 : supported(loader).contains(version->version());
        if (!allowed)
            continue;

        m_version->addItem(release ? version->version() : tr("%1 (snapshot)").arg(version->version()), version->version());
        if (release && latestIndex < 0) {
            latestIndex = m_version->count() - 1;
            m_latestRelease = version->version();
        }
    }

    int index = m_version->findData(previous);
    if (index < 0)
        index = latestIndex;
    m_version->setCurrentIndex(index);
}

const QSet<QString>& QuickplayDialog::supported(Quickplay::Loader loader)
{
    auto it = m_supported.find(loader);
    if (it == m_supported.end())
        it = m_supported.insert(loader, Quickplay::supportedMinecraftVersions(loader));
    return it.value();
}

QString QuickplayDialog::minecraftVersion() const
{
    return m_version->currentData().toString();
}

Quickplay::Preset QuickplayDialog::preset() const
{
    return m_presets.at(m_preset->currentIndex());
}

bool QuickplayDialog::isLatestRelease() const
{
    return !m_latestRelease.isEmpty() && minecraftVersion() == m_latestRelease;
}

void QuickplayDialog::run(QWidget* parent)
{
    const auto settings = APPLICATION->settings();
    const auto presets = Quickplay::loadPresets();
    if (presets.isEmpty()) {
        showError(parent, tr("There is no preset to use. Add one in the Quickplay settings."));
        return;
    }
    const auto minecraft = APPLICATION->metadataIndex()->get(MinecraftUid);

    // Fabric is needed for the oldest version to offer, the others for what the loader of a preset supports.
    QList<Quickplay::Loader> loaders{ Quickplay::Loader::Fabric };
    for (const auto& preset : presets) {
        if (preset.loader != Quickplay::Loader::None && !loaders.contains(preset.loader))
            loaders.append(preset.loader);
    }
    auto lists = QList<Meta::VersionList::Ptr>{ minecraft };
    for (const auto loader : loaders)
        lists.append(Quickplay::loaderLists(loader));
    if (std::any_of(lists.begin(), lists.end(), [](const auto& list) { return !list; })) {
        showError(parent, tr("The version metadata is not available."));
        return;
    }

    ConcurrentTask::Ptr loading(new ConcurrentTask(tr("Loading version lists")));
    QList<shared_qobject_ptr<Quickplay::PackVersionsTask>> packTasks;  // what the modpack presets have files for
    for (const auto& preset : presets) {
        if (preset.kind == Quickplay::Kind::Modpack) {
            packTasks.append(makeShared<Quickplay::PackVersionsTask>(preset.pack));
            loading->addTask(packTasks.last());
        }
    }
    QSet<Meta::VersionList*> queued;  // Fabric and Quilt share the intermediary list
    for (const auto& list : lists) {
        if (!list->isLoaded() && !queued.contains(list.get())) {
            queued.insert(list.get());
            loading->addTask(list->getLoadTask());
        }
    }
    if ((!queued.isEmpty() || !packTasks.isEmpty()) && !runModal(parent, loading.get()))
        return;

    QHash<QString, QSet<QString>> packVersions;
    for (qsizetype i = 0, task = 0; i < presets.size(); i++) {
        if (presets.at(i).kind == Quickplay::Kind::Modpack)
            packVersions.insert(presets.at(i).id, packTasks.at(task++)->versions());
    }

    QuickplayDialog dialog(presets, minecraft, packVersions, parent);
    if (dialog.exec() != QDialog::Accepted)
        return;

    const QString version = dialog.minecraftVersion();
    const Quickplay::Preset preset = dialog.preset();
    settings->set(PresetSetting, preset.id);
    settings->set(VersionSetting, dialog.isLatestRelease() ? QString() : version);  // empty = follow the latest release

    // A modpack preset has an instance of its own, the others share one.
    const auto prepare = [&](auto task) -> QString {
        if (!runModal(parent, task.get()))
            return {};
        if (const auto warnings = task->warnings(); !warnings.isEmpty())
            CustomMessageBox::selectable(parent, tr("Warnings"), warnings.join('\n'), QMessageBox::Warning)->show();
        return task->instanceId();
    };
    const QString instanceId = preset.kind == Quickplay::Kind::Modpack ? prepare(makeShared<Quickplay::PackPrepareTask>(version, preset))
                                                                       : prepare(makeShared<Quickplay::PrepareTask>(version, preset));
    if (instanceId.isEmpty())
        return;

    auto* instance = APPLICATION->instances()->getInstanceById(instanceId);
    if (!instance) {
        showError(parent, tr("The Quickplay instance could not be found."));
        return;
    }
    APPLICATION->launch(instance);
}
