// SPDX-License-Identifier: GPL-3.0-only
/*
 *  Prism Launcher - Minecraft Launcher
 *  Copyright (c) 2022 flowln <flowlnlnln@gmail.com>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, version 3.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QList>
#include <QMovie>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTextObjectInterface>
#include <QUrl>
#include <memory>

#include "net/NetJob.h"

/** Custom image text object to be used instead of the normal one in ProjectDescriptionPage.
 *
 *  Why? Because we want to re-scale images dynamically based on the document's size, in order to
 *  not have images being weirdly cropped out in different resolutions.
 */
class VariableSizedImageObject final : public QObject, public QTextObjectInterface {
    Q_OBJECT
    Q_INTERFACES(QTextObjectInterface)

    struct ImageMetadata {
        int posInDocument = 0;
        QUrl url;
        QImage image;
        int width = 0;
        int height = 0;
    };

    /** A download in progress and every place of the document that waits for its image (pickle-descriptions). */
    struct PendingImage {
        QPointer<NetJob> job;
        QList<std::shared_ptr<ImageMetadata>> targets;
    };

   public:
    QSizeF intrinsicSize(QTextDocument* doc, int posInDocument, const QTextFormat& format) override;
    void drawObject(QPainter* painter, const QRectF& rect, QTextDocument* doc, int posInDocument, const QTextFormat& format) override;

    void setMetaEntry(QString meta_entry) { m_meta_entry = meta_entry; }

    /** Starts downloading every image of the document that has not been loaded yet (pickle-descriptions).
     *
     *  Without this an image is only fetched when it is first painted, i.e. when it is scrolled into view,
     *  and the page jumps when it arrives. The same address is downloaded once, however often it is used.
     */
    void preload(QTextDocument* doc);

    /** Pauses every animation while the page is not shown, and lets them go on when it is (pickle-descriptions). */
    void setSuspended(bool suspended);

   signals:
    /** An animated image has a new frame: the view has to be painted again (pickle-descriptions). */
    void animationFrame();

   public slots:
    /** Stops all currently loading images from modifying the document, and aborts their downloads.
     *
     *  Call it before the content of the document is replaced.
     */
    void flush();

   private:
    /** Adds the image to the document, in the given position.
     */
    void parseImage(QTextDocument* doc, std::shared_ptr<ImageMetadata> meta);

    /** Makes the image appear in the document: joins the download of the same address if there is one, else starts it.
     */
    void requestImage(QTextDocument* doc, std::shared_ptr<ImageMetadata> meta);

    /** Loads an image from an external source, and adds it to the document.
     *
     *  This uses m_meta_entry to cache the image.
     */
    void loadImage(QTextDocument* doc, std::shared_ptr<ImageMetadata> meta);

    /** Plays the cached file as an animation if it has several frames (pickle-descriptions).
     *
     *  The key is the image name of the document format, so every place that uses the address shares one movie.
     */
    void startAnimation(const QString& key, const QString& path);

    /** An animation that is on the page: it only plays while it is painted (pickle-descriptions). */
    struct Animation {
        QPointer<QMovie> movie;
        bool painted = true;  // painted since the last frame we asked the view to show
        QElapsedTimer sinceRepaint;
    };

   private:
    QString m_meta_entry;

    QHash<QString, Animation> m_animations;
    bool m_suspended = false;

    QHash<QUrl, PendingImage> m_pending;
    /** Raised by flush(); the callbacks of an older generation must not touch the document. */
    unsigned m_generation = 0;
};
