// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackPage.h"

#include <QCheckBox>
#include <QClipboard>
#include <QFile>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTextBrowser>

#include "Application.h"
#include "BaseInstance.h"
#include "sharedpack/SharedPackCredentials.h"
#include "sharedpack/SharedPackCheck.h"
#include "sharedpack/SharedPackTasks.h"
#include "ui/dialogs/CustomMessageBox.h"
#include "ui/SharedPackUi.h"
#include "ui_ManagedPackPage.h"


SharedPackPage::SharedPackPage(BaseInstance* inst, QWidget* parent) : ManagedPackPage(inst, nullptr, parent)
{
    // Parts of the managed pack page that mean something for Modrinth and CurseForge only.
    ui->versionsComboBox->hide();
    ui->urlLine->hide();
    ui->updateFromFileButton->hide();
    ui->updateToVersionLabel->hide();
    ui->packOriginLabel->hide();
    ui->packOrigin->hide();
    ui->updateButton->setText(tr("Update Pack"));
    ui->updateButton->setDisabled(true);
    connect(ui->updateButton, &QPushButton::clicked, this, &SharedPackPage::update);

    m_autoUpdate = new QCheckBox(tr("Auto-Update"), this);
    m_autoUpdate->setToolTip(tr("Install new revisions without asking when the launcher starts."));
    m_copyInvite = new QPushButton(tr("Copy Invite"), this);
    m_leave = new QPushButton(tr("Leave Pack"), this);
    m_publish = new QPushButton(tr("Publish"), this);
    m_publish->setVisible(false);

    auto* row = new QHBoxLayout();
    row->addWidget(m_autoUpdate);
    row->addStretch();
    row->addWidget(m_publish);
    row->addWidget(m_copyInvite);
    row->addWidget(m_leave);
    if (auto* grid = qobject_cast<QGridLayout*>(layout()))
        grid->addLayout(row, 1, 0);

    connect(m_autoUpdate, &QCheckBox::toggled, this, [this](bool on) {
        auto link = SharedPack::readLink(m_inst->instanceRoot());
        if (!link)
            return;
        link->autoUpdate = on;
        if (!SharedPack::writeLink(m_inst->instanceRoot(), *link)) {
            const QSignalBlocker blocker(m_autoUpdate);
            m_autoUpdate->setChecked(!on);
            SharedPackUi::showMessage(this, tr("Error"), tr("Cannot save the invite in the instance folder."), QMessageBox::Critical);
        }
    });
    connect(m_copyInvite, &QPushButton::clicked, this, &SharedPackPage::copyInvite);
    connect(m_leave, &QPushButton::clicked, this, &SharedPackPage::leave);
    connect(m_publish, &QPushButton::clicked, this, [this] { SharedPackUi::publish(this, m_inst->id()); });
}

QString SharedPackPage::displayName() const
{
    return tr("Shared Pack");
}

QIcon SharedPackPage::icon() const
{
    return QIcon::fromTheme("export");
}

void SharedPackPage::setPackControlsEnabled(bool enabled)
{
    m_autoUpdate->setEnabled(enabled);
    m_copyInvite->setEnabled(enabled);
    m_leave->setEnabled(enabled);
    m_publish->setEnabled(enabled);
}

void SharedPackPage::openedImpl()
{
    ui->packName->setText(m_inst->getManagedPackName());
    ui->packVersion->setText(m_inst->getManagedPackVersionName());
    ui->reloadButton->setVisible(false);
    m_manifest.reset();
    ui->updateButton->setDisabled(true);

    const auto link = SharedPack::readLink(m_inst->instanceRoot());
    setPackControlsEnabled(link.has_value());
    if (!link) {
        ui->changelogTextBrowser->setPlainText(tr("This instance is no longer linked to a pack."));
        return;
    }
    {
        const QSignalBlocker blocker(m_autoUpdate);
        m_autoUpdate->setChecked(link->autoUpdate);
    }
    startCheck(*link);
}

void SharedPackPage::startCheck(const SharedPack::Link& link)
{
    if (m_check && m_check->isRunning())
        m_check->abort();
    m_check.reset();

    const auto state = SharedPack::readStateOrFresh(SharedPack::layoutFor(m_inst->instanceRoot(), m_inst->gameRoot()), link.invite.packId);
    if (!state) {
        ui->changelogTextBrowser->setPlainText(tr("The saved state of this pack is damaged or belongs to another pack."));
        return;
    }
    // The owner is the one instance the credentials record names; "owner" in a state file is not proof (it is copied with the instance).
    const auto record = SharedPack::findOwnerRecord(APPLICATION->dataRoot(), link.invite.packId);
    const bool isOwner = record && record->instanceId == m_inst->id();
    m_publish->setVisible(isOwner);
    if (isOwner) {
        ui->changelogTextBrowser->setPlainText(tr("You published this pack, so it never updates from the host."));
        return;
    }

    ui->changelogTextBrowser->setPlainText(tr("Checking for updates..."));
    auto* check = new SharedPack::CheckTask(link.invite, *state);
    m_check = Task::Ptr(check);
    // Results of a check that was replaced meanwhile are ignored.
    connect(check, &Task::succeeded, this, [this, check, state] {
        if (m_check.get() != check)
            return;
        m_inst->setUpdateAvailable(check->updateAvailable());
        if (!check->updateAvailable()) {
            if (!state->changelog.isEmpty()) {
                ui->changelogTextBrowser->setPlainText(
                    tr("This pack is up to date at revision %1.").arg(state->revision) + QStringLiteral("\n\n") + state->changelog);
            } else {
                ui->changelogTextBrowser->setPlainText(tr("This pack is up to date."));
            }
            return;
        }
        m_manifest = *check->manifest();
        // The changelog is text written by the owner: shown as plain text, never as HTML.
        const QString changelog = m_manifest->changelog.isEmpty() ? tr("No changelog.") : m_manifest->changelog;
        ui->changelogTextBrowser->setPlainText(tr("Revision %1").arg(m_manifest->revision) + QStringLiteral("\n\n") + changelog);
        ui->updateButton->setDisabled(false);
    });
    connect(check, &Task::failed, this, [this, check](QString reason) {
        if (m_check.get() != check)
            return;
        ui->changelogTextBrowser->setPlainText(reason);
        ui->reloadButton->setVisible(true);
    });
    check->start();
}

void SharedPackPage::update()
{
    if (!m_manifest)
        return;
    SharedPackUi::runUpdate(this, m_inst->id(), *m_manifest);
    openedImpl();  // new revision on the label, and a fresh check
}

void SharedPackPage::copyInvite()
{
    const auto link = SharedPack::readLink(m_inst->instanceRoot());
    if (link)
        QGuiApplication::clipboard()->setText(SharedPack::inviteToString(link->invite));
}

void SharedPackPage::leave()
{
    const auto answer = CustomMessageBox::selectable(this, tr("Leave Pack"),
                                                     tr("The instance and its files stay, but it stops following this pack and gets no "
                                                        "more updates. You need the invite to join again."),
                                                     QMessageBox::Question, QMessageBox::Yes | QMessageBox::No)
                            ->exec();
    if (answer != QMessageBox::Yes)
        return;

    if (m_check && m_check->isRunning())
        m_check->abort();
    m_check.reset();
    m_manifest.reset();
    m_inst->setUpdateAvailable(false);
    QFile::remove(SharedPack::linkPath(m_inst->instanceRoot()));
    QFile::remove(SharedPack::layoutFor(m_inst->instanceRoot(), m_inst->gameRoot()).stateFile);

    // BaseInstance::setManagedPack can only turn the flag on: clear it the way it stores it.
    auto* settings = m_inst->settings();
    settings->set("ManagedPack", false);
    for (const char* key : { "ManagedPackType", "ManagedPackID", "ManagedPackName", "ManagedPackVersionID", "ManagedPackVersionName" })
        settings->set(key, QString());

    setPackControlsEnabled(false);
    ui->updateButton->setDisabled(true);
    ui->changelogTextBrowser->setPlainText(tr("You left this pack."));
}
