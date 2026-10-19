// SPDX-License-Identifier: GPL-3.0-only

#include "PackGallery.h"

#include <QCoreApplication>
#include <QJsonObject>
#include <QStringList>
#include <QUrl>
#include <algorithm>

namespace ModPlatform {

namespace {

bool isWebAddress(const QString& address)
{
    const QUrl url(address);
    return url.isValid() && (url.scheme() == "http" || url.scheme() == "https") && !url.host().isEmpty();
}

// A title like "image-attachment_<uuid>.png" or "Default Image" says nothing, it is what the site fills in
bool isUsefulTitle(const QString& title)
{
    const QString trimmed = title.trimmed();
    if (trimmed.isEmpty() || trimmed.compare("Default Image", Qt::CaseInsensitive) == 0)
        return false;

    static const QStringList s_extensions{ ".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp" };
    return std::none_of(s_extensions.cbegin(), s_extensions.cend(),
                        [&trimmed](const QString& extension) { return trimmed.endsWith(extension, Qt::CaseInsensitive); });
}

}  // namespace

QList<GalleryImage> parseModrinthGallery(const QJsonArray& gallery)
{
    struct Entry {
        GalleryImage image;
        bool featured;
        int ordering;
    };

    QList<Entry> entries;
    for (const auto& value : gallery) {
        const auto obj = value.toObject();

        // "url" is the smaller version, "raw_url" the original
        const QString url = obj["url"].toString();
        const QString rawUrl = obj["raw_url"].toString();
        const QString thumbnail = isWebAddress(url) ? url : rawUrl;
        const QString full = isWebAddress(rawUrl) ? rawUrl : url;
        if (!isWebAddress(thumbnail) || !isWebAddress(full))
            continue;

        entries.append({ { thumbnail, full, obj["title"].toString() }, obj["featured"].toBool(), obj["ordering"].toInt() });
    }

    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.featured != b.featured)
            return a.featured;
        return a.ordering < b.ordering;
    });

    QList<GalleryImage> images;
    for (const auto& entry : entries)
        images.append(entry.image);
    return images;
}

QList<GalleryImage> parseCurseForgeScreenshots(const QJsonArray& screenshots)
{
    QList<GalleryImage> images;
    for (const auto& value : screenshots) {
        const auto obj = value.toObject();

        const QString url = obj["url"].toString();
        const QString thumbnail = obj["thumbnailUrl"].toString();
        const QString shown = isWebAddress(thumbnail) ? thumbnail : url;
        if (!isWebAddress(url) || !isWebAddress(shown))
            continue;

        images.append({ shown, url, obj["title"].toString() });
    }
    return images;
}

QString galleryHtml(const QList<GalleryImage>& images, int maxImages)
{
    if (images.isEmpty() || maxImages <= 0)
        return {};

    QString html = "<hr><h3>" + QCoreApplication::translate("ModPlatform", "Gallery").toHtmlEscaped() + "</h3>";
    for (const auto& image : images.first(std::min<qsizetype>(images.size(), maxImages))) {
        html += QString("<p><a href=\"%1\"><img src=\"%2\"></a>").arg(image.url.toHtmlEscaped(), image.thumbnailUrl.toHtmlEscaped());
        if (isUsefulTitle(image.title))
            html += "<br>" + image.title.trimmed().toHtmlEscaped();
        html += "</p>";
    }
    return html;
}

}  // namespace ModPlatform
