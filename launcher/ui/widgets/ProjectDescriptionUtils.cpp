// SPDX-License-Identifier: GPL-3.0-only
/*
 *  picklelauncher - helpers for the project description views (pickle-descriptions)
 */

#include "ProjectDescriptionUtils.h"

#include <QImageReader>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>

namespace ProjectDescription {

QList<ImageRef> imageRefs(QTextDocument* doc)
{
    QList<ImageRef> refs;
    for (QTextBlock block = doc->begin(); block.isValid(); block = block.next()) {
        for (auto it = block.begin(); !it.atEnd(); ++it) {
            const auto fragment = it.fragment();
            const auto format = fragment.charFormat();
            if (!format.isImageFormat())
                continue;

            const QUrl url(format.property(QTextFormat::ImageName).toString());
            if (url.isEmpty())
                continue;

            auto dimension = [&format](int property) {
                const auto value = format.property(property);
                return value.isValid() ? value.toInt() : 0;
            };
            for (int i = 0; i < fragment.length(); ++i)
                refs.append(
                    { fragment.position() + i, url, dimension(QTextFormat::ImageWidth), dimension(QTextFormat::ImageHeight), format });
        }
    }
    return refs;
}

bool isAnimatedImage(const QString& path, qint64 maxPixels)
{
    QImageReader reader(path);
    if (!reader.canRead() || !reader.supportsAnimation())
        return false;

    const QSize size = reader.size();
    if (!size.isValid() || qint64(size.width()) * size.height() > maxPixels)
        return false;

    return reader.imageCount() > 1;
}

QTextCharFormat charFormatAt(QTextDocument* doc, int position)
{
    QTextCursor cursor(doc);
    cursor.setPosition(position);
    cursor.setPosition(position + 1, QTextCursor::KeepAnchor);
    return cursor.charFormat();
}

QString resolveLinkouts(const QString& html)
{
    // href="/linkout?remoteUrl=https%253a%252f%252fexample.com%252f" (either quote, more parameters are ignored)
    static const QRegularExpression s_linkout(R"((href\s*=\s*)(["'])/linkout\?remoteUrl=([^"'&]+)[^"']*\2)",
                                              QRegularExpression::CaseInsensitiveOption);

    QString result;
    qsizetype copied = 0;
    for (auto it = s_linkout.globalMatch(html); it.hasNext();) {
        const auto match = it.next();
        result += html.mid(copied, match.capturedStart() - copied);
        copied = match.capturedEnd();

        const auto once = QUrl::fromPercentEncoding(match.captured(3).toUtf8());
        const QUrl target(QUrl::fromPercentEncoding(once.toUtf8()));
        if (target.isValid() && (target.scheme() == "http" || target.scheme() == "https"))
            result += match.captured(1) + match.captured(2) + target.toString(QUrl::FullyEncoded).toHtmlEscaped() + match.captured(2);
        else
            result += match.captured(0);
    }
    return result + html.mid(copied);
}

}  // namespace ProjectDescription
