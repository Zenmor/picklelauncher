// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackState.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <cmath>

#include "SharedPackPath.h"

namespace SharedPack {

namespace {

constexpr qint64 MaxStateBytes = 4 * 1024 * 1024;

Parsed<State> corrupt(const QString& why)
{
    Parsed<State> result;
    result.failure = Failure::Corrupt;
    result.error = why;
    return result;
}

bool isIntIn(const QJsonValue& value, qint64 min, qint64 max)
{
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    return number == std::floor(number) && number >= static_cast<double>(min) && number <= static_cast<double>(max);
}

}  // namespace

Parsed<State> parseState(const QByteArray& json)
{
    if (json.size() > MaxStateBytes)
        return corrupt(QStringLiteral("state file too large"));
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return corrupt(parseError.error != QJsonParseError::NoError ? parseError.errorString() : QStringLiteral("not an object"));
    const QJsonObject object = document.object();

    if (!isIntIn(object.value("formatVersion"), 1, Limits::MaxRevision))
        return corrupt(QStringLiteral("bad formatVersion"));
    State state;
    state.formatVersion = object.value("formatVersion").toInt();
    if (state.formatVersion > FormatVersion) {
        // Written by a newer launcher. Reading on could lose what it recorded, so the caller must leave the file alone.
        Parsed<State> result;
        result.failure = Failure::TooNew;
        result.error = QStringLiteral("format version %1 is newer than the supported %2").arg(state.formatVersion).arg(FormatVersion);
        return result;
    }

    state.packId = object.value("packId").toString();
    if (!isValidPackId(state.packId))
        return corrupt(QStringLiteral("bad packId"));
    const QString role = object.value("role").toString();
    if (role == QLatin1String("member"))
        state.role = Role::Member;
    else if (role == QLatin1String("owner"))
        state.role = Role::Owner;
    else
        return corrupt(QStringLiteral("bad role"));
    if (!isIntIn(object.value("revision"), 0, Limits::MaxRevision))
        return corrupt(QStringLiteral("bad revision"));
    state.revision = object.value("revision").toInt();

    const QJsonValue installed = object.value("installed");
    if (!installed.isObject() || installed.toObject().size() > Limits::MaxFiles)
        return corrupt(QStringLiteral("bad installed list"));
    QSet<QString> lowerPaths;
    const QJsonObject files = installed.toObject();
    for (auto it = files.begin(); it != files.end(); ++it) {
        const QString path = it.key();
        // The planner deletes by these paths, so they get the same checks as the ones in a manifest.
        if (!isSafePath(path) || classify(path) == FileClass::Excluded || !it.value().isString() || !isValidSha512(it.value().toString()))
            return corrupt(QStringLiteral("bad entry in the installed list"));
        if (lowerPaths.contains(path.toLower()))
            return corrupt(QStringLiteral("two installed files with the same path"));
        lowerPaths.insert(path.toLower());
        state.installed.insert(path, it.value().toString());
    }

    const QString changelog = object.value("changelog").toString();
    if (changelog.size() > Limits::MaxChangelogChars)
        return corrupt(QStringLiteral("changelog too long"));
    state.changelog = changelog;

    Parsed<State> result;
    result.value = std::move(state);
    return result;
}

QByteArray stateToJson(const State& state)
{
    QJsonObject installed;
    for (auto it = state.installed.cbegin(); it != state.installed.cend(); ++it)
        installed.insert(it.key(), it.value());

    QJsonObject object;
    object.insert("formatVersion", state.formatVersion);
    object.insert("packId", state.packId);
    object.insert("role", state.role == Role::Owner ? QStringLiteral("owner") : QStringLiteral("member"));
    object.insert("revision", state.revision);
    object.insert("installed", installed);
    if (!state.changelog.isEmpty())
        object.insert("changelog", state.changelog);
    return QJsonDocument(object).toJson(QJsonDocument::Indented);
}

UpdateCheck checkForUpdate(const State& state, const Latest& latest)
{
    if (state.role == Role::Owner)
        return UpdateCheck::OwnerNeverUpdates;
    if (latest.packId != state.packId)
        return UpdateCheck::WrongPack;
    if (latest.revision < state.revision)
        return UpdateCheck::HostBehind;
    return latest.revision > state.revision ? UpdateCheck::Available : UpdateCheck::UpToDate;
}

}  // namespace SharedPack
