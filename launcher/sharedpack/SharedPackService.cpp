// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackService.h"

#include <QTimer>

#include "Application.h"
#include "InstanceList.h"
#include "SharedPackCheck.h"
#include "SharedPackCredentials.h"
#include "SharedPackLink.h"
#include "SharedPackOwner.h"
#include "SharedPackTasks.h"
#include "minecraft/MinecraftInstance.h"

namespace SharedPack {

Service::Service(QObject* parent) : QObject(parent) {}

void Service::checkAll()
{
    if (m_running)
        return;
    m_pending.clear();
    const auto* list = APPLICATION->instances();
    for (int i = 0; i < list->count(); ++i) {
        const auto* instance = dynamic_cast<const MinecraftInstance*>(list->at(i));
        if (instance && readLink(instance->instanceRoot()))
            m_pending.append(instance->id());
    }
    m_running = true;
    next();
}

void Service::scheduleNext()
{
    // Task::emitSucceeded/emitFailed still touch the task after the signal: it must not be dropped (m_current is replaced by the next
    // round) from inside its own handler, so the round goes on from the event loop.
    QTimer::singleShot(0, this, [this] { next(); });
}

void Service::next()
{
    // Instances can come and go while a round runs: everything is looked up again for each one.
    while (!m_pending.isEmpty()) {
        const QString id = m_pending.takeFirst();
        auto* instance = dynamic_cast<MinecraftInstance*>(APPLICATION->instances()->getInstanceById(id));
        if (!instance)
            continue;
        const auto link = readLink(instance->instanceRoot());
        if (!link)
            continue;
        auto state = readStateOrFresh(layoutFor(instance->instanceRoot(), instance->gameRoot()), link->invite.packId);
        if (!state) {
            emit checkFailed(id, tr("The saved state of this pack is damaged or belongs to another pack."));
            continue;
        }
        // The owner role belongs to the one instance the credentials name, not to whatever state file says so: a copy of the owner's
        // instance, or one imported from another computer, is a member (it has a link) and updates like one.
        const auto owner = findOwnerRecord(APPLICATION->dataRoot(), link->invite.packId);
        const Standing standing = standingOf(id, owner ? std::optional<QString>(owner->instanceId) : std::nullopt, true);
        if (const auto fixed = reconcileState(*state, standing))
            writeState(layoutFor(instance->instanceRoot(), instance->gameRoot()), *fixed);
        if (standing == Standing::Owner)
            continue;  // the source instance never receives updates
        if (state->role == Role::Owner)
            state->role = Role::Member;

        auto* check = new CheckTask(link->invite, *state);
        m_current = Task::Ptr(check);
        connect(check, &Task::succeeded, this, [this, id, check, autoUpdate = link->autoUpdate] {
            if (!check->updateAvailable()) {
                scheduleNext();
                return;
            }
            const Manifest manifest = *check->manifest();
            const auto* current = APPLICATION->instances()->getInstanceById(id);
            if (autoUpdate && current && !current->isRunning())
                startAutoUpdate(id, manifest);
            else {
                emit updateAvailable(id, manifest);
                scheduleNext();
            }
        });
        connect(check, &Task::failed, this, [this, id, check](QString reason) {
            if (check->failure() == CheckFailure::LinkInvalid)
                emit linkInvalid(id);
            else
                emit checkFailed(id, reason);
            scheduleNext();
        });
        connect(check, &Task::aborted, this, [this] { scheduleNext(); });
        check->start();
        return;  // the rest of the round continues from the handlers above
    }
    m_running = false;
    m_current.reset();
    m_updating.reset();
    emit finished();
}

void Service::startAutoUpdate(const QString& instanceId, const Manifest& manifest)
{
    const auto* instance = APPLICATION->instances()->getInstanceById(instanceId);
    const auto link = instance ? readLink(instance->instanceRoot()) : std::nullopt;
    if (!link) {
        scheduleNext();
        return;
    }
    // The check task that called us is still on the stack of its own signal: m_current is replaced from the event loop only.
    auto* update = new UpdateTask(instanceId, link->invite, manifest);
    m_updating = Task::Ptr(update);
    const int revision = manifest.revision;
    connect(update, &Task::succeeded, this, [this, instanceId, revision, update] {
        emit autoUpdated(instanceId, revision, update->result());
        scheduleNext();
    });
    connect(update, &Task::failed, this, [this, instanceId](QString reason) {
        emit autoUpdateFailed(instanceId, reason);
        scheduleNext();
    });
    connect(update, &Task::aborted, this, [this] { scheduleNext(); });
    update->start();
}

}  // namespace SharedPack
