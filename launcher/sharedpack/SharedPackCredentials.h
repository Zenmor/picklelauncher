// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QMap>
#include <QString>
#include <optional>

#include "SharedPackOwner.h"

namespace SharedPack {

/// What this launcher knows about a pack it owns. Kept apart from the instance: instances are copied, exported and shared, this is not.
struct OwnerRecord {
    QString instanceId;  ///< the ONE instance of this launcher that is the owner (see standingOf)
    HostConfig host;
};

struct Credentials {
    QMap<QString, OwnerRecord> packs;  ///< by packId
    QMap<QString, QString> publisherTokens;  ///< by relay Worker URL
    QString lastPublisherWorker;
};

/// <dataDir>/sharedpack-credentials.json. Holds the owner's write keys in clear text, like every other launcher token on disk (accounts,
/// API keys): it is never in instance.cfg, never in an instance folder, never in an export, and every value in it is registered with the
/// log redactor before a request is made (registerSecrets).
QString credentialsPath(const QString& dataDir);

/// A missing file is an empty set; a damaged one (not JSON, a record the validator refuses) is std::nullopt, and writeOwnerRecord
/// refuses to replace it so one bad byte cannot cost the keys of the other packs.
std::optional<Credentials> readCredentials(const QString& dataDir);

/// Reads, replaces or adds the record of \a packId and writes the file atomically (temporary file, then rename; owner-only permissions
/// where the platform has them). False when the file is damaged or could not be written.
bool writeOwnerRecord(const QString& dataDir, const QString& packId, const OwnerRecord& record);

/// The record of \a packId, empty when there is none or the file is damaged. Registers its secrets with the redactor.
std::optional<OwnerRecord> findOwnerRecord(const QString& dataDir, const QString& packId);
std::optional<QString> findPublisherToken(const QString& dataDir, const QString& workerUrl);
std::optional<QString> findLastPublisherWorker(const QString& dataDir);
/// The server the last redeemed Publisher Code belongs to, but only while this computer still holds its publisher token: that is the case
/// in which the dialog needs no code.
std::optional<QString> findSavedPublisherWorker(const QString& dataDir);
bool writePublisherToken(const QString& dataDir, const QString& workerUrl, const QString& token);

/// The credentials as the bytes of the file, and back. Public for the tests.
QByteArray credentialsToJson(const Credentials& credentials);
std::optional<Credentials> parseCredentials(const QByteArray& json);

}  // namespace SharedPack
