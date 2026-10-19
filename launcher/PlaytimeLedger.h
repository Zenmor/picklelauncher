// SPDX-License-Identifier: GPL-3.0-only
/*
 *  picklelauncher - remembers the playtime of deleted instances (pickle-playtime)
 *
 *  The playtime of an instance is a setting of the instance, so it disappears with the instance. The ledger keeps
 *  the time of DELETED instances only; instances that still exist are read from their own settings, so nothing is
 *  ever counted twice. Time is kept per modpack (type + project id: every version, update and reinstall of one pack
 *  is the same pack). Instances that are not a modpack have no identity, their time is kept as one anonymous amount.
 */

#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>

class PlaytimeLedger {
   public:
    /** "modrinth:AANobbMI" for a pack, empty when the type or the id is empty (an instance without a pack). */
    static QString packKey(const QString& packType, const QString& packId);

    /** What was added to the ledger for one instance, to take it out again (instance restored from the trash). */
    struct Record {
        QString packKey;  // empty: the anonymous amount
        qint64 seconds = 0;
    };

    /** Adds seconds to a pack, or to the anonymous amount when the key is empty. Zero and negative amounts are ignored. */
    void add(const QString& packKey, qint64 seconds, const QString& packName = {});

    /** Takes seconds out again. Never goes below zero; a pack that reaches zero is dropped. */
    void subtract(const QString& packKey, qint64 seconds);

    qint64 packSeconds(const QString& packKey) const;

    /** One pack as the ledger remembers it. */
    struct PackEntry {
        QString key;
        QString name;  // as it was when the time was added, may be empty
        qint64 seconds = 0;
    };
    /** All packs with their time, in no particular order. Read-only: the anonymous amount is not in here. */
    QList<PackEntry> packEntries() const;

    qint64 anonymousSeconds() const { return m_anonymous; }
    /** Everything the ledger remembers: all packs plus the anonymous amount. */
    qint64 totalSeconds() const;
    bool isEmpty() const { return m_anonymous == 0 && m_packs.isEmpty(); }

    QJsonObject toJson() const;
    /** Fails (returns false, the ledger is left empty) for anything that is not a version 1 ledger. */
    bool fromJson(const QJsonObject& json);

    enum class LoadResult {
        Missing,  // no file: an empty ledger, fine
        Loaded,
        FromBackup,  // the file was unreadable, the .bak was used
        Unusable,    // a file exists but neither it nor its .bak can be read: leave the files alone
    };
    /** Reads the ledger. The ledger is empty unless the result is Loaded or FromBackup. */
    LoadResult load(const QString& path);

    /** Writes the ledger atomically; the previous file is kept as "<path>.bak". */
    bool save(const QString& path) const;

    bool operator==(const PlaytimeLedger&) const = default;

   private:
    struct Pack {
        qint64 seconds = 0;
        QString name;
        bool operator==(const Pack&) const = default;
    };

    qint64 m_anonymous = 0;
    QHash<QString, Pack> m_packs;
};
