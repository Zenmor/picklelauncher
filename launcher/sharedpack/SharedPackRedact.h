// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <QString>
#include <QStringList>

namespace SharedPack {

/// Removes secrets (invite tokens, access keys) from text before it is logged. Not thread-safe: the owner of the instance guards it.
class Redactor {
   public:
    /// Secrets shorter than this are ignored: replacing a short common string everywhere would make the log useless.
    static constexpr int MinSecretChars = 8;

    /// Registers \a secret, and its percent-encoded spelling when that differs (URLs).
    void add(const QString& secret);

    QString apply(QString text) const;

   private:
    QStringList m_needles;  ///< longest first, so a secret that contains another is removed whole
};

/// The launcher-wide redactor behind the log (see appDebugOutput). Both functions are thread-safe and never log.
/// Secrets stay registered until the launcher exits: a pack that was left may still appear in a late log line.
void registerLogSecret(const QString& secret);
QString redactForLog(QString text);

}  // namespace SharedPack
