// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackSelection.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <algorithm>

#include "SharedPackManifest.h"
#include "SharedPackPath.h"

namespace SharedPack {

namespace {

constexpr qint64 MaxSelectionBytes = 1024 * 1024;
constexpr int MaxEntries = 5000;
constexpr int MaxNameChars = 255;

QStringList readStrings(const QJsonValue& value, bool onlySafePaths)
{
    QStringList result;
    for (const QJsonValue& entry : value.toArray()) {
        if (result.size() >= MaxEntries)
            break;
        const QString text = entry.toString();
        if (text.isEmpty() || text.size() > MaxNameChars * 4)
            continue;
        if (onlySafePaths ? isSafePath(text) : text.size() <= MaxNameChars)
            result.append(text);
    }
    return result;
}

QStringList sortedUnique(QStringList list)
{
    list.removeDuplicates();
    std::sort(list.begin(), list.end(), [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
    return list;
}

}  // namespace

QStringList defaultTickedFolders()
{
    return { QStringLiteral("mods"),     QStringLiteral("config"),        QStringLiteral("defaultconfigs"), QStringLiteral("kubejs"),
             QStringLiteral("scripts"),  QStringLiteral("datapacks"),     QStringLiteral("resourcepacks"),  QStringLiteral("shaderpacks") };
}

QStringList defaultUnticked(const QStringList& topLevelNames)
{
    QSet<QString> ticked;
    for (const QString& name : defaultTickedFolders())
        ticked.insert(name.toLower());
    QStringList result;
    for (const QString& name : topLevelNames) {
        if (!name.isEmpty() && !ticked.contains(name.toLower()))
            result.append(name);
    }
    return result;
}

QStringList initialUnticked(const std::optional<StoredSelection>& stored, const QStringList& topLevelNames, bool keepEverythingTicked)
{
    if (!stored)
        return keepEverythingTicked ? QStringList() : defaultUnticked(topLevelNames);

    QSet<QString> known;
    for (const QString& name : stored->known)
        known.insert(name.toLower());
    QStringList unseen;
    for (const QString& name : topLevelNames) {
        if (!known.contains(name.toLower()))
            unseen.append(name);
    }
    return sortedUnique(stored->unticked + defaultUnticked(unseen));
}

QString selectionPath(const QString& instanceRoot)
{
    return QDir::cleanPath(instanceRoot) + QStringLiteral("/sharedpack-selection.json");
}

std::optional<StoredSelection> readSelection(const QString& instanceRoot)
{
    QFile file(selectionPath(instanceRoot));
    if (!file.open(QIODevice::ReadOnly) || file.size() > MaxSelectionBytes)
        return std::nullopt;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject())
        return std::nullopt;
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("formatVersion")).toInt(0) != 1)
        return std::nullopt;
    StoredSelection selection;
    selection.unticked = readStrings(object.value(QStringLiteral("unticked")), true);
    selection.known = readStrings(object.value(QStringLiteral("known")), false);
    return selection;
}

bool writeSelection(const QString& instanceRoot, const StoredSelection& selection)
{
    const QJsonObject object{ { QStringLiteral("formatVersion"), 1 },
                              { QStringLiteral("unticked"), QJsonArray::fromStringList(sortedUnique(selection.unticked)) },
                              { QStringLiteral("known"), QJsonArray::fromStringList(sortedUnique(selection.known)) } };
    QSaveFile file(selectionPath(instanceRoot));
    return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(object).toJson(QJsonDocument::Indented)) >= 0 && file.commit();
}

}  // namespace SharedPack
