// SPDX-License-Identifier: GPL-3.0-only

#include "ui/dialogs/PublishSharedPackDialog.h"
#include "SharedPackUi.h"

#include <QDebug>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <memory>

#include "Application.h"
#include "BaseInstance.h"
#include "InstanceList.h"
#include "QObjectPtr.h"
#include "sharedpack/SharedPackLink.h"
#include "sharedpack/SharedPackService.h"
#include "sharedpack/SharedPackTasks.h"
#include "ui/dialogs/CustomMessageBox.h"
#include "ui/dialogs/JoinSharedPackDialog.h"
#include "ui/dialogs/ProgressDialog.h"

namespace {

constexpr int MaxListedFiles = 20;

/// "- a\n- b ... and 3 more": a pack can hold thousands of files, a message box must not grow with it.
QString listOf(const QStringList& files)
{
    QStringList lines;
    for (int i = 0; i < files.size() && i < MaxListedFiles; ++i)
        lines.append(QStringLiteral("- ") + files.at(i));
    if (files.size() > MaxListedFiles)
        lines.append(QObject::tr("... and %1 more").arg(files.size() - MaxListedFiles));
    return lines.join('\n');
}

QString nameOf(const QString& instanceId)
{
    const auto* instance = APPLICATION->instances()->getInstanceById(instanceId);
    return instance ? instance->name() : instanceId;
}

/// The update badge of the instance list. Runtime only (not saved): the next start-up check sets it again.
void setUpdateBadge(const QString& instanceId, bool on)
{
    if (auto* instance = APPLICATION->instances()->getInstanceById(instanceId))
        instance->setUpdateAvailable(on);
}

/// What one start-up round found; shown when it is over, so no dialog opens while the service still runs.
struct Round {
    QList<std::pair<QString, SharedPack::Manifest>> updates;
    QList<std::pair<QString, SharedPack::ApplyResult>> installed;
    QStringList problems;
};

void finishRound(QWidget* window, Round& round)
{
    for (const auto& [id, result] : round.installed)
        SharedPackUi::reportResult(window, nameOf(id), result);
    if (!round.problems.isEmpty())
        SharedPackUi::showMessage(window, QObject::tr("Shared Pack"), round.problems.join(QStringLiteral("\n")), QMessageBox::Warning);

    for (const auto& [id, manifest] : round.updates) {
        auto* box = CustomMessageBox::selectable(window, QObject::tr("Update Pack"),
                                                 QObject::tr("%1 has a new revision (%2). Update it now?").arg(nameOf(id)).arg(manifest.revision),
                                                 QMessageBox::Question, QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
        box->setTextFormat(Qt::PlainText);
        box->setDetailedText(manifest.changelog.isEmpty() ? QObject::tr("No changelog.") : manifest.changelog);
        const int answer = box->exec();
        delete box;
        if (answer == QMessageBox::Yes)
            SharedPackUi::runUpdate(window, id, manifest);
    }
}

}  // namespace

namespace SharedPackUi {

void showMessage(QWidget* parent, const QString& title, const QString& text, QMessageBox::Icon icon)
{
    auto* box = CustomMessageBox::selectable(parent, title, text, icon);
    box->setTextFormat(Qt::PlainText);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->show();
}

void reportResult(QWidget* parent, const QString& instanceName, const SharedPack::ApplyResult& result)
{
    QStringList notes;
    if (!result.packNew.isEmpty())
        notes.append(QObject::tr("You changed these files and the pack changed them too. Yours were kept, the pack's version is next to "
                                 "each one with the extra ending .packnew:") +
                     QStringLiteral("\n") + listOf(result.packNew));
    if (!result.skipped.isEmpty())
        notes.append(QObject::tr("These files of the pack were not written because their path is not safe:") + QStringLiteral("\n") +
                     listOf(result.skipped));
    if (notes.isEmpty())
        return;
    showMessage(parent, instanceName, notes.join(QStringLiteral("\n\n")), QMessageBox::Warning);
}

bool runUpdate(QWidget* parent, const QString& instanceId, const SharedPack::Manifest& manifest)
{
    const auto* instance = APPLICATION->instances()->getInstanceById(instanceId);
    const auto link = instance ? SharedPack::readLink(instance->instanceRoot()) : std::nullopt;
    if (!link)
        return false;
    if (instance->isRunning()) {
        showMessage(parent, QObject::tr("Error"), QObject::tr("Close the game before updating this pack."), QMessageBox::Critical);
        return false;
    }

    unique_qobject_ptr<SharedPack::UpdateTask> task(new SharedPack::UpdateTask(instanceId, link->invite, manifest));
    ProgressDialog dialog(parent);
    dialog.setSkipButton(true, QObject::tr("Abort"));
    dialog.execWithTask(task.get());

    if (task->wasSuccessful()) {
        setUpdateBadge(instanceId, false);
        reportResult(parent, nameOf(instanceId), task->result());
        return true;
    }
    if (task->getState() == Task::State::Failed)
        showMessage(parent, QObject::tr("Error"), task->failReason(), QMessageBox::Critical);
    return false;
}

void publish(QWidget* parent, const QString& instanceId)
{
    PublishSharedPackDialog dialog(instanceId, parent);
    dialog.exec();
}

void join(QWidget* parent)
{
    // The new instance is selected by the instance list itself when it is committed.
    JoinSharedPackDialog dialog(parent);
    dialog.exec();
}

void start(QWidget* window)
{
    auto* service = new SharedPack::Service(window);
    auto round = std::make_shared<Round>();
    const QPointer<QWidget> guard(window);

    QObject::connect(service, &SharedPack::Service::updateAvailable, service,
                     [round](const QString& id, const SharedPack::Manifest& manifest) {
                         round->updates.append({ id, manifest });
                         setUpdateBadge(id, true);
                     });
    QObject::connect(service, &SharedPack::Service::autoUpdated, service,
                     [round](const QString& id, int, const SharedPack::ApplyResult& result) {
                         setUpdateBadge(id, false);
                         if (!result.packNew.isEmpty() || !result.skipped.isEmpty())
                             round->installed.append({ id, result });
                     });
    QObject::connect(service, &SharedPack::Service::linkInvalid, service, [round](const QString& id) {
        round->problems.append(QObject::tr("%1: the host does not accept the invite any more. Ask the owner for a new one.").arg(nameOf(id)));
    });
    QObject::connect(service, &SharedPack::Service::autoUpdateFailed, service, [round](const QString& id, const QString& reason) {
        round->problems.append(nameOf(id) + QStringLiteral(": ") + reason);
    });
    // Being offline at start-up is normal: a failed check goes to the log only (the reason never holds the invite).
    QObject::connect(service, &SharedPack::Service::checkFailed, service, [](const QString& id, const QString& reason) {
        qWarning() << "Shared pack check failed for" << id << ":" << reason;
    });
    QObject::connect(service, &SharedPack::Service::finished, service, [round, guard] {
        if (guard)
            finishRound(guard, *round);
        *round = Round();
    });

    // After the window is up: the check runs in the background, the prompts come when it is over.
    QTimer::singleShot(0, service, [service] { service->checkAll(); });
}

}  // namespace SharedPackUi
