// SPDX-License-Identifier: GPL-3.0-only

#include "QuickplayPage.h"

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPixmapCache>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>

#include "Application.h"
#include "modplatform/ModIndex.h"
#include "modplatform/modrinth/ModrinthAPI.h"
#include "net/ApiDownload.h"
#include "net/HttpMetaCache.h"
#include "quickplay/QuickplayModPicker.h"
#include "settings/SettingsObject.h"
#include "ui/widgets/ModListView.h"
#include "ui/widgets/WideBar.h"

namespace {

const QString HideSetting = QStringLiteral("QuickplayHideInstance");
const QString IconCacheBase = QStringLiteral("ModrinthPacks");  // the cache the mod search of an instance uses, so icons are shared
const QSize ImageSize(32, 32);                                  // the size of the Image column of the instance's Mods tab

QString newPresetId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

}  // namespace

QuickplayPage::QuickplayPage(QWidget* parent) : QWidget(parent), m_presets(Quickplay::loadPresets())
{
    m_savedJson = Quickplay::presetsToJson(m_presets);

    const auto button = [this](const QString& text, void (QuickplayPage::*slot)()) {
        auto* b = new QPushButton(text, this);
        connect(b, &QPushButton::clicked, this, slot);
        return b;
    };

    m_hideInstance = new QCheckBox(tr("Hide the Quickplay instance from the instance list"), this);
    m_hideInstance->setChecked(APPLICATION->settings()->get(HideSetting).toBool());

    // One preset at a time: the combo box picks it, the rest of the box edits it.
    m_preset = new QComboBox(this);
    m_removePreset = button(tr("Remove"), &QuickplayPage::removePreset);

    auto* presetRow = new QHBoxLayout();
    presetRow->addWidget(m_preset, 1);
    presetRow->addWidget(button(tr("Add"), &QuickplayPage::addPreset));
    presetRow->addWidget(button(tr("Add Modpack"), &QuickplayPage::addModpackPreset));
    presetRow->addWidget(button(tr("Duplicate"), &QuickplayPage::duplicatePreset));
    presetRow->addWidget(m_removePreset);

    m_name = new QLineEdit(this);
    m_loader = new QComboBox(this);
    m_loader->addItem(Quickplay::loaderName(Quickplay::Loader::None), int(Quickplay::Loader::None));
    for (const auto loader : Quickplay::modLoaders())
        m_loader->addItem(Quickplay::loaderName(loader), int(loader));

    auto* form = m_form = new QFormLayout();
    form->addRow(tr("Preset"), presetRow);
    form->addRow(tr("Name"), m_name);
    form->addRow(tr("Mod Loader"), m_loader);
    // The mod list is the one of the instance's Mods tab, with the same toolbar on its right. It is not sortable: the order is the
    // order of the preset.
    m_modsModel = new QStandardItemModel(0, 3, this);
    m_modsModel->setHorizontalHeaderLabels({ tr("Image"), tr("Name"), tr("Provider") });
    m_mods = new ModListView(this);
    m_mods->setModel(m_modsModel);
    m_mods->setResizeModes({ QHeaderView::ResizeToContents, QHeaderView::Stretch, QHeaderView::ResizeToContents });
    m_mods->setSortingEnabled(false);
    m_mods->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_mods->setMinimumHeight(180);
    m_mods->setContextMenuPolicy(Qt::CustomContextMenu);

    const auto action = [this](const QString& text, const QString& toolTip) {
        auto* a = new QAction(text, this);
        a->setToolTip(toolTip);
        return a;
    };
    auto* download = m_downloadMods = action(tr("Download Mods"), tr("Download mods from online mod platforms"));
    m_replaceMod = action(tr("Replace"), {});
    m_removeMod = action(tr("&Remove"), tr("Remove all selected items."));
    m_moveUp = action(tr("Move &Up"), {});
    m_moveDown = action(tr("Move &Down"), {});
    m_viewHomepage = action(tr("View Homepage"), tr("View the homepages of all selected mods."));
    connect(download, &QAction::triggered, this, &QuickplayPage::addMod);
    connect(m_replaceMod, &QAction::triggered, this, &QuickplayPage::replaceMod);
    connect(m_removeMod, &QAction::triggered, this, &QuickplayPage::removeMods);
    connect(m_moveUp, &QAction::triggered, this, [this] { moveMod(-1); });
    connect(m_moveDown, &QAction::triggered, this, [this] { moveMod(1); });
    connect(m_viewHomepage, &QAction::triggered, this, &QuickplayPage::viewHomepage);

    auto* toolbar = new WideBar(this);
    toolbar->setOrientation(Qt::Vertical);
    toolbar->addAction(download);
    toolbar->addAction(m_replaceMod);
    toolbar->addSeparator();
    toolbar->addAction(m_removeMod);
    toolbar->addAction(m_moveUp);
    toolbar->addAction(m_moveDown);
    toolbar->addSeparator();
    toolbar->addAction(m_viewHomepage);

    m_modsPanel = new QWidget(this);
    auto* modsLayout = new QHBoxLayout(m_modsPanel);
    modsLayout->setContentsMargins(0, 0, 0, 0);
    modsLayout->addWidget(m_mods, 1);
    modsLayout->addWidget(toolbar, 0, Qt::AlignTop);

    auto* presetsBox = new QGroupBox(tr("Presets"), this);
    auto* presetsLayout = new QVBoxLayout(presetsBox);
    presetsLayout->addLayout(form);
    presetsLayout->addWidget(m_modsPanel, 1);
    // The content scrolls when the window is too small for it, instead of squeezing the lists away.
    auto* content = new QWidget(this);
    auto* layout = new QVBoxLayout(content);
    layout->addWidget(m_hideInstance);
    layout->addWidget(presetsBox, 1);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);

    connect(m_preset, &QComboBox::currentIndexChanged, this, &QuickplayPage::showPreset);
    connect(m_name, &QLineEdit::textEdited, this, [this](const QString& text) {
        const int row = m_preset->currentIndex();
        if (row < 0)
            return;
        m_presets[row].name = text.trimmed();
        m_preset->setItemText(row, m_presets[row].name);
    });
    connect(m_loader, &QComboBox::currentIndexChanged, this, [this] {
        const auto loader = Quickplay::Loader(m_loader->currentData().toInt());
        if (const int row = m_preset->currentIndex(); row >= 0)
            m_presets[row].loader = loader;
        m_modsPanel->setEnabled(loader != Quickplay::Loader::None);
    });
    connect(m_mods->selectionModel(), &QItemSelectionModel::selectionChanged, this, &QuickplayPage::updateModButtons);
    connect(m_mods, &QWidget::customContextMenuRequested, this, [this, toolbar](const QPoint& pos) {
        auto* menu = toolbar->createContextMenu(this, tr("Context menu"));
        menu->exec(m_mods->mapToGlobal(pos));
        delete menu;
    });

    refreshPresets(0);
}

bool QuickplayPage::apply()
{
    for (int row = 0; row < m_presets.size(); row++) {
        if (m_presets.at(row).name.isEmpty()) {
            m_preset->setCurrentIndex(row);
            QMessageBox::warning(this, tr("Quickplay"), tr("Every preset needs a name."));
            return false;
        }
    }

    const QByteArray json = Quickplay::presetsToJson(m_presets);
    if (json != m_savedJson) {
        if (!Quickplay::savePresets(m_presets)) {
            QMessageBox::warning(this, tr("Quickplay"), tr("The presets could not be saved."));
            return false;
        }
        m_savedJson = json;
    }

    APPLICATION->settings()->set(HideSetting, m_hideInstance->isChecked());
    return true;
}

void QuickplayPage::refreshPresets(int select)
{
    {
        const QSignalBlocker blocker(m_preset);
        m_preset->clear();
        for (const auto& preset : m_presets)
            m_preset->addItem(preset.name);
        m_preset->setCurrentIndex(select);
    }
    showPreset(select);
}
void QuickplayPage::showPreset(int row)
{
    if (row < 0 || row >= m_presets.size())
        return;
    const auto& preset = m_presets.at(row);

    m_removePreset->setEnabled(m_presets.size() > 1);  // there is always at least one preset to choose from

    {
        const QSignalBlocker nameBlocker(m_name);
        m_name->setText(preset.name);
    }
    {
        const QSignalBlocker loaderBlocker(m_loader);
        m_loader->setCurrentIndex(m_loader->findData(int(preset.loader)));
    }
    // A modpack brings its own loader and mods: the list shows the pack and the loader is not asked for.
    const bool isPack = preset.kind == Quickplay::Kind::Modpack;
    m_form->setRowVisible(m_loader, !isPack);
    m_modsPanel->setEnabled(isPack || preset.loader != Quickplay::Loader::None);
    showMods();
}

void QuickplayPage::addPreset()
{
    m_presets.append(Quickplay::Preset{ newPresetId(), tr("New preset"), Quickplay::Loader::Fabric, {} });
    refreshPresets(m_presets.size() - 1);
    m_name->setFocus();
    m_name->selectAll();
}

void QuickplayPage::addModpackPreset()
{
    const auto pack = Quickplay::pickPack(this);
    if (!pack)
        return;
    Quickplay::Preset preset{ newPresetId(), pack->name, Quickplay::Loader::None, {} };
    preset.kind = Quickplay::Kind::Modpack;
    preset.pack = *pack;
    m_presets.append(preset);
    refreshPresets(m_presets.size() - 1);
}

QList<Quickplay::Mod> QuickplayPage::shownMods(const Quickplay::Preset& preset)
{
    return preset.kind == Quickplay::Kind::Modpack ? QList<Quickplay::Mod>{ preset.pack } : preset.mods;
}

void QuickplayPage::duplicatePreset()
{
    const int row = m_preset->currentIndex();
    if (row < 0)
        return;
    auto copy = m_presets.at(row);
    copy.id = newPresetId();
    copy.name = tr("%1 (copy)").arg(copy.name);
    m_presets.append(copy);
    refreshPresets(m_presets.size() - 1);
}

void QuickplayPage::removePreset()
{
    const int row = m_preset->currentIndex();
    if (row < 0 || m_presets.size() <= 1)
        return;
    m_presets.removeAt(row);
    refreshPresets(qMin(row, m_presets.size() - 1));
}

void QuickplayPage::showMods(int select)
{
    m_modsModel->removeRows(0, m_modsModel->rowCount());  // not clear(), that removes the header labels too

    const int row = m_preset->currentIndex();
    if (row >= 0) {
        for (const auto& mod : shownMods(m_presets.at(row))) {
            auto* image = new QStandardItem();
            image->setData(pixmapOf(mod), Qt::DecorationRole);
            image->setSizeHint(ImageSize);
            auto* name = new QStandardItem(mod.name);
            name->setToolTip(mod.projectId);
            m_modsModel->appendRow({ image, name, new QStandardItem(ModPlatform::ProviderCapabilities::readableName(mod.provider)) });
        }
        if (select >= 0 && select < m_modsModel->rowCount())
            m_mods->setCurrentIndex(m_modsModel->index(select, 0));
    }

    updateModButtons();
    loadIcons();
}

QList<int> QuickplayPage::selectedMods() const
{
    QList<int> rows;
    for (const auto& index : m_mods->selectionModel()->selectedRows())
        rows.append(index.row());
    std::ranges::sort(rows);
    return rows;
}

int QuickplayPage::currentMod() const
{
    const auto rows = selectedMods();
    return rows.size() == 1 ? rows.first() : -1;
}

void QuickplayPage::updateModButtons()
{
    const auto rows = selectedMods();
    const bool one = rows.size() == 1;
    if (const int row = m_preset->currentIndex(); row >= 0 && m_presets.at(row).kind == Quickplay::Kind::Modpack) {
        // The list is the pack alone: it can be replaced and looked at, nothing else
        m_downloadMods->setEnabled(false);
        m_replaceMod->setEnabled(true);
        m_removeMod->setEnabled(false);
        m_moveUp->setEnabled(false);
        m_moveDown->setEnabled(false);
        m_viewHomepage->setEnabled(!rows.isEmpty());
        return;
    }
    m_downloadMods->setEnabled(true);
    m_replaceMod->setEnabled(one);
    m_removeMod->setEnabled(!rows.isEmpty());
    m_viewHomepage->setEnabled(!rows.isEmpty());
    m_moveUp->setEnabled(one && rows.first() > 0);
    m_moveDown->setEnabled(one && rows.first() < m_modsModel->rowCount() - 1);
}

// Taking one mod from both providers installs it once at best (see Quickplay::sameNameOtherProvider), so the user decides.
static QList<Quickplay::Mod> withoutDeclinedClashes(QWidget* parent, const QList<Quickplay::Mod>& existing, QList<Quickplay::Mod> picked)
{
    const auto clashes = Quickplay::sameNameOtherProvider(existing, picked);
    if (clashes.isEmpty())
        return picked;

    QStringList names;
    for (const auto& mod : clashes)
        names.append(mod.name);
    const auto answer = QMessageBox::question(
        parent, QuickplayPage::tr("Duplicate Mods"),
        QuickplayPage::tr("These mods are already in the preset from the other provider:\n\n%1\n\nAdd them anyway?").arg(names.join('\n')),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer == QMessageBox::Yes)
        return picked;

    picked.removeIf([&clashes](const Quickplay::Mod& mod) {
        return std::ranges::any_of(clashes, [&mod](const Quickplay::Mod& clash) { return clash.key() == mod.key(); });
    });
    return picked;
}

void QuickplayPage::addMod()
{
    const int row = m_preset->currentIndex();
    if (row < 0)
        return;

    auto& preset = m_presets[row];
    const auto picked = withoutDeclinedClashes(
        this, preset.mods, Quickplay::pickMods(this, preset.loader, preset.mods, tr("Add mods to %1").arg(preset.name)));
    if (picked.isEmpty())
        return;

    preset.mods.append(picked);
    showMods(preset.mods.size() - 1);
}

void QuickplayPage::replaceMod()
{
    const int row = m_preset->currentIndex();
    if (row >= 0 && m_presets.at(row).kind == Quickplay::Kind::Modpack) {
        if (const auto pack = Quickplay::pickPack(this)) {
            m_presets[row].pack = *pack;
            showMods(0);
        }
        return;
    }
    const int index = currentMod();
    if (row < 0 || index < 0)
        return;

    auto& preset = m_presets[row];
    const auto replaced = preset.mods.at(index);
    const auto picked = Quickplay::pickMods(this, preset.loader, preset.mods, tr("Replace %1").arg(replaced.name));
    if (picked.isEmpty())
        return;

    // The mod that goes does not count when looking for the same mod from the other provider. If more than one mod was chosen, they
    // all take its place.
    auto others = preset.mods;
    others.removeAt(index);
    const auto kept = withoutDeclinedClashes(this, others, picked);
    if (kept.isEmpty())
        return;

    preset.mods.removeAt(index);
    for (int i = 0; i < kept.size(); ++i)
        preset.mods.insert(index + i, kept.at(i));  // keeps the place in the order
    showMods(index);
}

void QuickplayPage::removeMods()
{
    const int row = m_preset->currentIndex();
    if (row < 0)
        return;

    auto indexes = selectedMods();
    if (indexes.isEmpty())
        return;
    std::ranges::reverse(indexes);  // from the back, so the remaining indexes stay valid
    for (const int index : indexes)
        m_presets[row].mods.removeAt(index);

    showMods(qMin(indexes.last(), int(m_presets.at(row).mods.size()) - 1));
}

void QuickplayPage::moveMod(int offset)
{
    const int row = m_preset->currentIndex();
    const int index = currentMod();
    if (row < 0 || index < 0)
        return;

    auto& mods = m_presets[row].mods;
    if (index + offset < 0 || index + offset >= mods.size())
        return;
    mods.swapItemsAt(index, index + offset);
    showMods(index + offset);
}

void QuickplayPage::viewHomepage()
{
    const int row = m_preset->currentIndex();
    if (row < 0)
        return;
    const auto mods = shownMods(m_presets.at(row));
    for (const int index : selectedMods()) {
        const auto& mod = mods.at(index);
        QDesktopServices::openUrl(QUrl(ModPlatform::getMetaURL(mod.provider, mod.projectId)));
    }
}

QPixmap QuickplayPage::pixmapOf(const Quickplay::Mod& mod) const
{
    QPixmap pixmap;
    if (!mod.iconUrl.isEmpty() && QPixmapCache::find(mod.iconUrl, &pixmap))
        return pixmap.scaled(ImageSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return QIcon::fromTheme("screenshot-placeholder").pixmap(ImageSize);
}

void QuickplayPage::refreshIcons()
{
    const int row = m_preset->currentIndex();
    if (row < 0)
        return;
    const auto mods = shownMods(m_presets.at(row));
    for (int i = 0; i < m_modsModel->rowCount() && i < mods.size(); i++)
        m_modsModel->item(i, 0)->setData(pixmapOf(mods.at(i)), Qt::DecorationRole);
}

// Downloads the icons of the shown mods that are not loaded yet, and asks Modrinth for the icon of the mods that have none.
void QuickplayPage::loadIcons()
{
    const int row = m_preset->currentIndex();
    if (row < 0)
        return;

    QStringList withoutIcon;
    QList<QUrl> missing;
    for (const auto& mod : shownMods(m_presets.at(row))) {
        if (mod.iconUrl.isEmpty()) {
            if (mod.provider == ModPlatform::ResourceProvider::MODRINTH)  // only Modrinth can tell the icon of a project ID alone
                withoutIcon.append(mod.projectId);
            continue;
        }
        QPixmap pixmap;
        const QUrl url(mod.iconUrl);
        if (!QPixmapCache::find(mod.iconUrl, &pixmap) && !m_requestedIcons.contains(url)) {
            m_requestedIcons.insert(url);
            missing.append(url);
        }
    }

    fetchProjects(withoutIcon);
    if (missing.isEmpty())
        return;

    auto job = makeShared<NetJob>(QStringLiteral("Quickplay mod icons"), APPLICATION->network());
    job->setAskRetry(false);
    for (const auto& url : missing) {
        const auto entry = APPLICATION->metacache()->resolveEntry(
            IconCacheBase,
            QString("logos/%1").arg(QString(QCryptographicHash::hash(url.toEncoded(), QCryptographicHash::Algorithm::Sha1).toHex())));
        const auto action = Net::ApiDownload::makeCached(url, entry);
        const QString path = entry->getFullPath();
        connect(action.get(), &Task::succeeded, this, [this, url, path] {
            const QIcon icon(path);
            QPixmapCache::insert(url.toString(), icon.pixmap(icon.actualSize({ 64, 64 })));
            refreshIcons();
        });
        job->addNetAction(action);
    }
    m_iconJobs.append(job);
    job->start();
}

// The mods of presets from before there were icons only have a project ID, and those are all Modrinth ones.
void QuickplayPage::fetchProjects(const QStringList& ids)
{
    QStringList wanted;
    for (const auto& id : ids) {
        if (!m_requestedProjects.contains(id)) {
            m_requestedProjects.insert(id);
            wanted.append(id);
        }
    }
    if (wanted.isEmpty())
        return;

    const auto request = ModrinthAPI().getProjects(wanted);
    const Task::Ptr task = request.first;
    const QByteArray* response = request.second;
    connect(task.get(), &Task::succeeded, this, [this, response] {
        const auto document = QJsonDocument::fromJson(*response);
        if (!document.isArray())
            return;

        bool changed = false;
        for (const auto& value : document.array()) {
            const auto project = value.toObject();
            const QString id = project.value("id").toString();
            const QString icon = project.value("icon_url").toString();
            if (icon.isEmpty())
                continue;
            for (auto& preset : m_presets) {
                const auto setIcon = [&](Quickplay::Mod& mod) {
                    if (mod.provider == ModPlatform::ResourceProvider::MODRINTH && mod.projectId == id && mod.iconUrl != icon) {
                        mod.iconUrl = icon;
                        changed = true;
                    }
                };
                for (auto& mod : preset.mods)
                    setIcon(mod);
                if (preset.kind == Quickplay::Kind::Modpack)
                    setIcon(preset.pack);
            }
        }
        if (changed) {
            refreshIcons();  // the ones that are loaded already
            loadIcons();     // downloads the others, the rows get them when they are there
        }
    });
    m_projectTasks.append(task);
    task->start();
}
