// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackLink.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace SharedPack {

QString linkPath(const QString& instanceRoot)
{
    return QDir::cleanPath(instanceRoot) + QStringLiteral("/sharedpack-link.json");
}

Layout layoutFor(const QString& instanceRoot, const QString& gameDir)
{
    const QString root = QDir::cleanPath(instanceRoot);
    Layout layout;
    layout.gameDir = QDir::cleanPath(gameDir);
    layout.stateFile = root + QStringLiteral("/sharedpack-state.json");
    layout.workDir = root + QStringLiteral("/sharedpack-tmp");
    return layout;
}

std::optional<Link> readLink(const QString& instanceRoot)
{
    QFile file(linkPath(instanceRoot));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 2 * MaxInviteChars)
        return std::nullopt;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return std::nullopt;
    const auto invite = parseInvite(document.object().value(QStringLiteral("invite")).toString());
    if (!invite)
        return std::nullopt;
    Link link;
    link.invite = *invite.value;
    link.autoUpdate = document.object().value(QStringLiteral("autoUpdate")).toBool(false);
    return link;
}

bool writeLink(const QString& instanceRoot, const Link& link)
{
    const QJsonObject object{ { QStringLiteral("invite"), inviteToString(link.invite) },
                              { QStringLiteral("autoUpdate"), link.autoUpdate } };
    QSaveFile file(linkPath(instanceRoot));
    return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(object).toJson(QJsonDocument::Indented)) >= 0 && file.commit();
}

std::optional<State> readStateOrFresh(const Layout& layout, const QString& packId)
{
    QFile file(layout.stateFile);
    if (!file.exists()) {
        State fresh;
        fresh.packId = packId;
        return fresh;
    }
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const auto parsed = parseState(file.readAll());
    if (!parsed || parsed.value->packId != packId)
        return std::nullopt;
    return *parsed.value;
}

bool writeState(const Layout& layout, const State& state)
{
    QSaveFile file(layout.stateFile);
    return file.open(QIODevice::WriteOnly) && file.write(stateToJson(state)) >= 0 && file.commit();
}

}  // namespace SharedPack
