// SPDX-License-Identifier: GPL-3.0-only

#include "SharedPackRedact.h"

#include <QMutex>
#include <QMutexLocker>
#include <QUrl>
#include <algorithm>
#include <utility>

namespace SharedPack {

void Redactor::add(const QString& secret)
{
    if (secret.size() < MinSecretChars)
        return;
    for (const QString& needle : { secret, QString::fromLatin1(QUrl::toPercentEncoding(secret)) }) {
        if (!m_needles.contains(needle))
            m_needles.append(needle);
    }
    std::sort(m_needles.begin(), m_needles.end(), [](const QString& a, const QString& b) { return a.size() > b.size(); });
}

QString Redactor::apply(QString text) const
{
    for (const QString& needle : m_needles)
        text.replace(needle, QStringLiteral("[redacted]"));
    return text;
}

namespace {
QMutex& logMutex()
{
    static QMutex mutex;
    return mutex;
}
Redactor& logRedactor()
{
    static Redactor redactor;
    return redactor;
}
}  // namespace

void registerLogSecret(const QString& secret)
{
    const QMutexLocker locker(&logMutex());
    logRedactor().add(secret);
}

QString redactForLog(QString text)
{
    const QMutexLocker locker(&logMutex());
    return logRedactor().apply(std::move(text));
}

}  // namespace SharedPack
