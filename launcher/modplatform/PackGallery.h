// SPDX-License-Identifier: GPL-3.0-only
/*
 *  picklelauncher - screenshots of a mod or modpack for its description (pickle-descriptions)
 */

#pragma once

#include <QJsonArray>
#include <QList>
#include <QString>

namespace ModPlatform {

/** One screenshot of a project page. */
struct GalleryImage {
    QString thumbnailUrl;  // shown in the description
    QString url;           // opened when the thumbnail is clicked
    QString title;         // may be empty
};

/** The "gallery" array of a Modrinth project (GET /v2/project/{id}). Featured images come first, then by "ordering".
 *  Entries without an http(s) address are dropped.
 */
QList<GalleryImage> parseModrinthGallery(const QJsonArray& gallery);

/** The "screenshots" array of a CurseForge mod. Entries without an http(s) address are dropped. */
QList<GalleryImage> parseCurseForgeScreenshots(const QJsonArray& screenshots);

/** The description part with the screenshots: a "Gallery" heading and a linked thumbnail per image (at most
 *  @p maxImages). Empty when there are no images. Titles that are only file names ("image-attachment_1.png") are not shown.
 */
QString galleryHtml(const QList<GalleryImage>& images, int maxImages = 10);

}  // namespace ModPlatform
