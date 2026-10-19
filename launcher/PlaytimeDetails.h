// SPDX-License-Identifier: GPL-3.0-only
/*
 *  picklelauncher - what the Playtime dialog shows about one row (pickle-playtime)
 *
 *  Pure merging, no GUI and no instance list: the caller reads the instances and hands over plain records, this works
 *  out the dates (including the guess for an instance that never recorded its first launch), the names of loaders and
 *  sources and the list of instances. Nothing in here is translated: the results are proper names, dates or states,
 *  the dialog turns the states into words.
 */

#pragma once

#include <QList>
#include <QString>

namespace PlaytimeDetails {

/** One instance that still exists. Time values are milliseconds since the epoch, 0 means unknown. */
struct InstanceRecord {
    QString name;
    QString packVersion;       // empty for an instance that is not a modpack
    QString minecraftVersion;  // empty: not known (components not loaded)
    QString loaderUid;         // uid of the mod loader component, empty: no loader (or not known, see minecraftVersion)
    QString loaderVersion;
    qint64 firstLaunchMs = 0;  // recorded by the launcher
    qint64 lastLaunchMs = 0;
    qint64 folderBirthMs = 0;  // creation time of the instance folder, the base of the guess
    qint64 seconds = 0;        // time played
};

/** What is known about a point in time. */
struct Moment {
    enum class State {
        NeverPlayed,
        NotRecorded,  // played, but nothing tells when
        Known,
    };
    State state = State::NeverPlayed;
    qint64 ms = 0;           // only for Known
    bool estimated = false;  // only for Known: taken from the creation time of the instance folder

    bool operator==(const Moment&) const = default;
};

/** One instance as the pane shows it. */
struct InstanceBlock {
    QString name;
    QString packVersion;
    QString minecraftVersion;
    QString loaderName;  // proper name, empty: no loader
    QString loaderVersion;
};

struct Input {
    QString name;
    QString packType;  // as the launcher stores it ("modrinth", "flame", ...), empty: not a modpack
    QList<InstanceRecord> instances;
    qint64 deletedSeconds = 0;  // time the ledger remembers for this pack
};

struct Details {
    QString name;
    QString sourceName;  // empty: not a modpack
    bool isPack = false;
    Moment firstPlayed;  // earliest over all instances
    Moment lastPlayed;   // latest over all instances
    QList<InstanceBlock> instances;
    qint64 deletedSeconds = 0;  // at least 0
};

/** Name of a mod loader by its component uid ("net.minecraftforge" gives "Forge"). Empty for a uid that is no mod loader. */
QString loaderName(const QString& loaderUid);

/** Name of a place a pack comes from ("flame" gives "CurseForge"). A type the launcher does not know is returned as it is. */
QString sourceName(const QString& packType);

/** First launch of one instance: the recorded time, else the guess, else why there is none. */
Moment firstPlayed(const InstanceRecord& instance);

Details build(const Input& input);

}  // namespace PlaytimeDetails
