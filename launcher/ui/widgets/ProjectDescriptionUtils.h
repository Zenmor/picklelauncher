// SPDX-License-Identifier: GPL-3.0-only
/*
 *  picklelauncher - helpers for the project description views (pickle-descriptions)
 */

#pragma once

#include <QList>
#include <QString>
#include <QTextCharFormat>
#include <QUrl>

class QTextDocument;

namespace ProjectDescription {

/** One image of a document: where it is, where it comes from and the size the HTML gave it (0 = not given). */
struct ImageRef {
    int position = 0;
    QUrl url;
    int width = 0;
    int height = 0;
    QTextCharFormat format;
};

/** Lists every image of the document in order, one entry per image character (equal neighbours share a text fragment,
 *  so the fragments alone would miss some). Images without an address are skipped.
 */
QList<ImageRef> imageRefs(QTextDocument* doc);

/** Largest animation we play: pixels of one frame, and size of the file (kept in memory while it plays). */
constexpr qint64 kMaxAnimationPixels = 4'000'000;
constexpr qint64 kMaxAnimationBytes = 16 * 1024 * 1024;

/** True when the image file has more than one frame (GIF, animated WebP) and its frames are small enough to animate.
 *
 *  The format is detected from the content, the cached files have no extension. An image whose frames are larger than
 *  @p maxPixels stays a still picture: decoding and repainting every frame of a huge animation would cost more than
 *  it is worth in a description. APNG is not animated by Qt's PNG plugin, so it stays a still picture too.
 */
bool isAnimatedImage(const QString& path, qint64 maxPixels = kMaxAnimationPixels);

/** Returns the format of the character at the given position.
 *
 *  QTextCursor::charFormat() of a collapsed cursor is the format of the character *before* the cursor
 *  (only at the start of a block it is the character after it). For a linked image (<a href><img></a>)
 *  that is usually a space, so the link was lost and only the first image of a line stayed clickable.
 *  Selecting the character first makes charFormat() return its own format.
 */
QTextCharFormat charFormatAt(QTextDocument* doc, int position);

/** CurseForge routes the outgoing links of a description through "/linkout?remoteUrl=<address, percent-encoded twice>".
 *  Those are relative, so a QTextBrowser can neither open nor even resolve them. This replaces them with the real
 *  http(s) address. Links that are not of that form (or point to another scheme) are left alone.
 */
QString resolveLinkouts(const QString& html);

}  // namespace ProjectDescription
