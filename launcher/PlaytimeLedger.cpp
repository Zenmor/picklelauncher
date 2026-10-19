// SPDX-License-Identifier: GPL-3.0-only

#include "PlaytimeLedger.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <algorithm>
#include <limits>

namespace {

constexpr int kVersion = 1;

// One amount from the file: a whole number of seconds, not negative
bool readSeconds(const QJsonValue& value, qint64& seconds)
{
    if (!value.isDouble())
        return false;
    const double number = value.toDouble();
    if (number < 0 || number != static_cast<double>(static_cast<qint64>(number)) || number > 1e15)
        return false;
    seconds = static_cast<qint64>(number);
    return true;
}

}  // namespace

QString PlaytimeLedger::packKey(const QString& packType, const QString& packId)
{
    if (packType.isEmpty() || packId.isEmpty())
        return {};
    return packType + ':' + packId;
}

void PlaytimeLedger::add(const QString& packKey, qint64 seconds, const QString& packName)
{
    if (seconds <= 0)
        return;

    // never overflow, whatever is in the settings of an instance
    auto sum = [](qint64 a, qint64 b) { return a > std::numeric_limits<qint64>::max() - b ? std::numeric_limits<qint64>::max() : a + b; };

    if (packKey.isEmpty()) {
        m_anonymous = sum(m_anonymous, seconds);
        return;
    }
    auto& pack = m_packs[packKey];
    pack.seconds = sum(pack.seconds, seconds);
    if (!packName.isEmpty())
        pack.name = packName;
}

void PlaytimeLedger::subtract(const QString& packKey, qint64 seconds)
{
    if (seconds <= 0)
        return;

    if (packKey.isEmpty()) {
        m_anonymous = std::max<qint64>(0, m_anonymous - seconds);
        return;
    }
    auto pack = m_packs.find(packKey);
    if (pack == m_packs.end())
        return;
    pack->seconds = std::max<qint64>(0, pack->seconds - seconds);
    if (pack->seconds == 0)
        m_packs.erase(pack);
}

qint64 PlaytimeLedger::packSeconds(const QString& packKey) const
{
    if (packKey.isEmpty())
        return 0;
    return m_packs.value(packKey).seconds;
}

QList<PlaytimeLedger::PackEntry> PlaytimeLedger::packEntries() const
{
    QList<PackEntry> entries;
    entries.reserve(m_packs.size());
    for (auto pack = m_packs.constBegin(); pack != m_packs.constEnd(); ++pack)
        entries.append({ pack.key(), pack.value().name, pack.value().seconds });
    return entries;
}

qint64 PlaytimeLedger::totalSeconds() const
{
    qint64 total = m_anonymous;
    for (const auto& pack : m_packs)
        total = total > std::numeric_limits<qint64>::max() - pack.seconds ? std::numeric_limits<qint64>::max() : total + pack.seconds;
    return total;
}

QJsonObject PlaytimeLedger::toJson() const
{
    QJsonObject packs;
    for (auto it = m_packs.cbegin(); it != m_packs.cend(); ++it) {
        QJsonObject pack{ { "seconds", static_cast<double>(it->seconds) } };
        if (!it->name.isEmpty())
            pack["name"] = it->name;
        packs[it.key()] = pack;
    }
    return { { "version", kVersion }, { "anonymous", static_cast<double>(m_anonymous) }, { "packs", packs } };
}

bool PlaytimeLedger::fromJson(const QJsonObject& json)
{
    PlaytimeLedger loaded;
    if (json["version"].toInt(-1) != kVersion)
        return false;
    if (!readSeconds(json["anonymous"], loaded.m_anonymous))
        return false;
    if (!json["packs"].isObject())
        return false;

    const auto packs = json["packs"].toObject();
    for (auto it = packs.begin(); it != packs.end(); ++it) {
        if (it.key().isEmpty() || !it.value().isObject())
            return false;
        const auto obj = it.value().toObject();
        Pack pack;
        if (!readSeconds(obj["seconds"], pack.seconds))
            return false;
        pack.name = obj["name"].toString();
        loaded.m_packs.insert(it.key(), pack);
    }

    *this = loaded;
    return true;
}

PlaytimeLedger::LoadResult PlaytimeLedger::load(const QString& path)
{
    *this = {};

    auto readFile = [this](const QString& file) {
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly))
            return false;
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(f.readAll(), &error);
        return error.error == QJsonParseError::NoError && doc.isObject() && fromJson(doc.object());
    };

    const bool mainExists = QFileInfo::exists(path);
    if (mainExists && readFile(path))
        return LoadResult::Loaded;

    const QString backup = path + ".bak";
    if (QFileInfo::exists(backup) && readFile(backup))
        return LoadResult::FromBackup;

    *this = {};
    return mainExists || QFileInfo::exists(backup) ? LoadResult::Unusable : LoadResult::Missing;
}

bool PlaytimeLedger::save(const QString& path) const
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return false;

    // Keep the previous file as a backup. It is only a convenience: if it cannot be made, the ledger is still written.
    const QString backup = path + ".bak";
    if (QFileInfo::exists(path)) {
        QFile::remove(backup);
        QFile::copy(path, backup);
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    if (file.write(QJsonDocument(toJson()).toJson(QJsonDocument::Indented)) < 0)
        return false;
    return file.commit();
}
