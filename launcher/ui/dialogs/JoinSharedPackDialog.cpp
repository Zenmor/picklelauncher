// SPDX-License-Identifier: GPL-3.0-only

#include "JoinSharedPackDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "PlaytimeDetails.h"
#include "QObjectPtr.h"
#include "StringUtils.h"
#include "sharedpack/SharedPackCheck.h"
#include "sharedpack/SharedPackState.h"
#include "sharedpack/SharedPackTasks.h"
#include "ui/SharedPackUi.h"
#include "ui/dialogs/ProgressDialog.h"

JoinSharedPackDialog::JoinSharedPackDialog(QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("Join Shared Pack"));
    resize(520, 420);

    m_invite = new QLineEdit(this);
    m_invite->setPlaceholderText(tr("Invite"));
    m_invite->setClearButtonEnabled(true);
    m_check = new QPushButton(tr("Check"), this);
    m_check->setEnabled(false);

    auto* inviteRow = new QHBoxLayout();
    inviteRow->addWidget(m_invite, 1);
    inviteRow->addWidget(m_check);

    m_summary = new QLabel(this);
    m_summary->setTextFormat(Qt::PlainText);  // the name comes from the owner
    m_summary->setWordWrap(true);
    m_changelog = new QPlainTextEdit(this);
    m_changelog->setReadOnly(true);
    m_changelog->setPlaceholderText(tr("Paste the invite your friend sent you, then press Check."));

    // Off by default: the pack's files come from someone else, so the member decides. The page of the instance can switch it later.
    m_autoUpdate = new QCheckBox(tr("Update automatically when the launcher starts"), this);
    m_autoUpdate->setToolTip(tr("You still see the changelog in the Shared Pack tab of the instance."));

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_join = buttons->button(QDialogButtonBox::Ok);
    m_join->setText(tr("Join"));
    m_join->setEnabled(false);

    auto* layout = new QVBoxLayout(this);
    layout->addLayout(inviteRow);
    layout->addWidget(m_summary);
    layout->addWidget(m_changelog, 1);
    layout->addWidget(m_autoUpdate);
    layout->addWidget(buttons);

    connect(m_invite, &QLineEdit::textChanged, this, &JoinSharedPackDialog::inviteChanged);
    connect(m_invite, &QLineEdit::returnPressed, this, [this] {
        if (m_check->isEnabled())
            check();
    });
    connect(m_check, &QPushButton::clicked, this, &JoinSharedPackDialog::check);
    connect(buttons, &QDialogButtonBox::accepted, this, &JoinSharedPackDialog::join);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // An invite that is already on the clipboard is filled in. Check contacts the host, so it is never pressed for the user.
    if (const QString fromClipboard = SharedPack::extractInvite(QApplication::clipboard()->text()); !fromClipboard.isEmpty()) {
        m_invite->setText(fromClipboard);
        m_summary->setText(tr("The invite was taken from the clipboard. Press Check to see the pack."));
    }
}

void JoinSharedPackDialog::inviteChanged()
{
    // Whatever was shown belongs to the previous text.
    m_manifest.reset();
    m_summary->clear();
    m_changelog->clear();
    m_join->setEnabled(false);

    m_parsed.reset();
    const QString text = m_invite->text().trimmed();
    if (!text.isEmpty()) {
        // The text may be a whole chat message: the invite is taken out of it, the strict parser still decides.
        auto parsed = SharedPack::parseInvite(SharedPack::extractInvite(text));
        if (parsed)
            m_parsed = std::move(parsed.value);
        else
            m_summary->setText(tr("This does not look like an invite. Paste the whole text you were sent."));  // the reason stays in the log
    }
    m_check->setEnabled(m_parsed.has_value());
}

void JoinSharedPackDialog::check()
{
    if (!m_parsed)
        return;
    m_summary->clear();
    SharedPack::State fresh;  // revision 0: whatever the host publishes counts as newer
    fresh.packId = m_parsed->packId;

    unique_qobject_ptr<SharedPack::CheckTask> task(new SharedPack::CheckTask(*m_parsed, fresh));
    ProgressDialog progress(this);
    progress.setSkipButton(true, tr("Abort"));
    progress.execWithTask(task.get());

    if (task->wasSuccessful() && task->updateAvailable()) {
        m_manifest = *task->manifest();
        showManifest(*m_manifest);
    } else if (task->getState() == Task::State::Failed) {
        const QString reason = task->failure() == SharedPack::CheckFailure::LinkInvalid
                                   ? tr("The host does not accept this invite. Ask the owner for a new one.")
                                   : task->failReason();
        m_summary->setText(reason);  // inline, where the invite is: nothing was started, the user fixes the text and presses Check again
    }
}

void JoinSharedPackDialog::showManifest(const SharedPack::Manifest& manifest)
{
    QStringList lines;
    lines.append(manifest.name + QStringLiteral(" - ") + tr("Revision %1").arg(manifest.revision));
    for (const auto& component : manifest.components) {
        const QString loader = PlaytimeDetails::loaderName(component.uid);
        const QString name = component.uid == QStringLiteral("net.minecraft") ? QStringLiteral("Minecraft") : loader;
        if (!name.isEmpty())
            lines.append(name + QStringLiteral(" ") + component.version);
    }
    lines.append(tr("%1 files, %2").arg(manifest.files.size()).arg(StringUtils::humanReadableFileSize(static_cast<double>(manifest.totalSize()))));
    m_summary->setText(lines.join(QStringLiteral("\n")));
    m_changelog->setPlainText(manifest.changelog.isEmpty() ? tr("No changelog.") : manifest.changelog);
    m_join->setEnabled(true);
}

void JoinSharedPackDialog::join()
{
    if (!m_parsed || !m_manifest)
        return;
    unique_qobject_ptr<SharedPack::JoinTask> task(new SharedPack::JoinTask(*m_parsed, *m_manifest, m_autoUpdate->isChecked()));
    ProgressDialog progress(this);
    progress.setSkipButton(true, tr("Abort"));
    progress.execWithTask(task.get());

    m_instanceId = task->instanceId();
    // The messages belong to the window behind: this dialog is gone as soon as it closes.
    QWidget* owner = parentWidget();
    if (task->wasSuccessful()) {
        SharedPackUi::reportResult(owner, m_manifest->name, task->result());
        accept();
        return;
    }
    if (task->getState() == Task::State::Failed)
        SharedPackUi::showMessage(m_instanceId.isEmpty() ? static_cast<QWidget*>(this) : owner, tr("Error"), task->failReason(),
                                  QMessageBox::Critical);
    if (!m_instanceId.isEmpty())
        accept();  // the instance exists and the next update of it finishes the job
}
