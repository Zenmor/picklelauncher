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

#include "VariableSizedImageObject.h"

#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QPainter>
#include <QTextObject>
#include <algorithm>
#include <memory>

#include "Application.h"
#include "ProjectDescriptionUtils.h"  // pickle-descriptions

#include "net/ApiDownload.h"
#include "net/NetJob.h"

enum FormatProperties { ImageData = QTextFormat::UserProperty + 1 };

QSizeF VariableSizedImageObject::intrinsicSize(QTextDocument* doc, int posInDocument, const QTextFormat& format)
{
    Q_UNUSED(posInDocument);

    auto image = qvariant_cast<QImage>(format.property(ImageData));
    auto size = image.size();

    // calculate the new image size based on the properties
    int width = 0;
    int height = 0;
    auto widthVar = format.property(QTextFormat::ImageWidth);
    if (widthVar.isValid()) {
        width = widthVar.toInt();
    }
    auto heigthVar = format.property(QTextFormat::ImageHeight);
    if (heigthVar.isValid()) {
        height = heigthVar.toInt();
    }

    if (size.isEmpty()) {
        // pickle-descriptions: before the image is loaded, keep the room the page asks for (both width and height),
        // so the text below does not jump when the image arrives. Otherwise we can't size an empty image.
        if (width <= 0 || height <= 0)
            return { size };
        size = { width, height };
    }

    if (width != 0 && height != 0) {
        size.setWidth(width);
        size.setHeight(height);
    } else if (width != 0) {
        size.setHeight((width * size.height()) / size.width());
        size.setWidth(width);
    } else if (height != 0) {
        size.setWidth((height * size.width()) / size.height());
        size.setHeight(height);
    }

    // Get the width of the text content to make the image similar sized.
    // doc->textWidth() includes the margin, so we need to remove it.
    auto doc_width = doc->textWidth() - 2 * doc->documentMargin();

    if (size.width() > doc_width)
        size *= doc_width / (double)size.width();

    return { size };
}

void VariableSizedImageObject::drawObject(QPainter* painter,
                                          const QRectF& rect,
                                          QTextDocument* doc,
                                          int posInDocument,
                                          const QTextFormat& format)
{
    if (!format.hasProperty(ImageData)) {
        QUrl image_url{ qvariant_cast<QString>(format.property(QTextFormat::ImageName)) };
        if (image_url.isEmpty())
            return;

        auto meta = std::make_shared<ImageMetadata>();
        meta->posInDocument = posInDocument;
        meta->url = image_url;

        auto widthVar = format.property(QTextFormat::ImageWidth);
        if (widthVar.isValid()) {
            meta->width = widthVar.toInt();
        }
        auto heigthVar = format.property(QTextFormat::ImageHeight);
        if (heigthVar.isValid()) {
            meta->height = heigthVar.toInt();
        }

        // pickle-descriptions: normally preload() has started this already, then this only joins it
        requestImage(doc, meta);
        return;
    }

    auto image = qvariant_cast<QImage>(format.property(ImageData));

    // pickle-descriptions: an animated image shows the frame of its movie, the stored image is only its first frame
    if (auto animation = m_animations.find(qvariant_cast<QString>(format.property(QTextFormat::ImageName)));
        animation != m_animations.end() && animation->movie) {
        animation->painted = true;
        if (!m_suspended && animation->movie->state() == QMovie::Paused)
            animation->movie->setPaused(false);  // it scrolled back into view

        if (const auto frame = animation->movie->currentImage(); !frame.isNull())
            image = frame;
    }

    painter->setRenderHint(QPainter::RenderHint::SmoothPixmapTransform);
    painter->drawImage(rect, image);
}

void VariableSizedImageObject::flush()
{
    // pickle-descriptions: a callback of an older generation must not edit the next document, even for the same address
    ++m_generation;
    for (const auto& pending : std::as_const(m_pending)) {
        if (pending.job && pending.job->isRunning())
            pending.job->abort();
    }
    m_pending.clear();

    // the animations belong to the old content too
    for (const auto& animation : std::as_const(m_animations)) {
        if (animation.movie)
            animation.movie->deleteLater();
    }
    m_animations.clear();
}

void VariableSizedImageObject::setSuspended(bool suspended)
{
    m_suspended = suspended;
    if (!suspended)
        return;  // a movie goes on when it is painted again

    for (const auto& animation : std::as_const(m_animations)) {
        if (animation.movie && animation.movie->state() == QMovie::Running)
            animation.movie->setPaused(true);
    }
}

void VariableSizedImageObject::startAnimation(const QString& key, const QString& path)
{
    if (m_animations.contains(key) || QFileInfo(path).size() > ProjectDescription::kMaxAnimationBytes ||
        !ProjectDescription::isAnimatedImage(path))
        return;

    // The movie reads from memory: a movie on the file would keep the cache entry open, and the meta cache could
    // not replace it (Windows) while the description is shown.
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return;

    auto* movie = new QMovie(this);
    auto* buffer = new QBuffer(movie);
    buffer->setData(file.readAll());
    movie->setDevice(buffer);
    if (!movie->isValid() || movie->frameCount() < 2) {
        delete movie;
        return;
    }

    m_animations.insert(key, { movie, true, {} });
    connect(movie, &QMovie::frameChanged, this, [this, key] {
        auto animation = m_animations.find(key);
        if (animation == m_animations.end() || !animation->movie)
            return;

        // Nobody has painted the last frame: it is off screen or the page is hidden, so stop decoding.
        // drawObject starts it again when the image comes back into view.
        if (!animation->painted || m_suspended) {
            animation->movie->setPaused(true);
            return;
        }

        // GIFs with tiny delays would repaint the whole view far more often than anyone can see
        constexpr qint64 kMinRepaintMs = 30;
        if (animation->sinceRepaint.isValid() && !animation->sinceRepaint.hasExpired(kMinRepaintMs))
            return;
        animation->sinceRepaint.restart();
        animation->painted = false;
        emit animationFrame();
    });
    movie->start();
}

void VariableSizedImageObject::preload(QTextDocument* doc)
{
    for (const auto& ref : ProjectDescription::imageRefs(doc)) {
        if (ref.format.hasProperty(ImageData))
            continue;

        auto meta = std::make_shared<ImageMetadata>();
        meta->posInDocument = ref.position;
        meta->url = ref.url;
        meta->width = ref.width;
        meta->height = ref.height;
        requestImage(doc, meta);
    }
}

void VariableSizedImageObject::parseImage(QTextDocument* doc, std::shared_ptr<ImageMetadata> meta)
{
    QTextCursor cursor(doc);
    cursor.setPosition(meta->posInDocument);
    cursor.setKeepPositionOnInsert(true);

    // pickle-descriptions: cursor.charFormat() would be the character before the image and lose its link
    auto image_char_format = ProjectDescription::charFormatAt(doc, meta->posInDocument);

    image_char_format.setObjectType(QTextFormat::ImageObject);
    image_char_format.setProperty(ImageData, meta->image);
    image_char_format.setProperty(QTextFormat::ImageName, meta->url.toDisplayString());
    image_char_format.setProperty(QTextFormat::ImageWidth, meta->width);
    image_char_format.setProperty(QTextFormat::ImageHeight, meta->height);

    // Qt doesn't allow us to modify the properties of an existing object in the document.
    // So we remove the old one and add the new one with the ImageData property set.
    cursor.deleteChar();
    cursor.insertText(QString(QChar::ObjectReplacementCharacter), image_char_format);
}

void VariableSizedImageObject::requestImage(QTextDocument* doc, std::shared_ptr<ImageMetadata> meta)
{
    if (meta->url.isEmpty())
        return;

    // pickle-descriptions: the same address used several times is downloaded once, every place is filled when it arrives
    if (auto pending = m_pending.find(meta->url); pending != m_pending.end()) {
        const bool known = std::any_of(pending->targets.cbegin(), pending->targets.cend(),
                                       [&meta](const auto& target) { return target->posInDocument == meta->posInDocument; });
        if (!known)
            pending->targets.append(meta);
        return;
    }

    loadImage(doc, meta);
}

void VariableSizedImageObject::loadImage(QTextDocument* doc, std::shared_ptr<ImageMetadata> meta)
{
    MetaEntryPtr entry = APPLICATION->metacache()->resolveEntry(
        m_meta_entry,
        QString("images/%1").arg(QString(QCryptographicHash::hash(meta->url.toEncoded(), QCryptographicHash::Algorithm::Sha1).toHex())));

    auto job = new NetJob(QString("Load Image: %1").arg(meta->url.fileName()), APPLICATION->network());
    job->setAskRetry(false);
    job->addNetAction(Net::ApiDownload::makeCached(meta->url, entry));

    // registered before the job starts: the callbacks below look the address up
    m_pending.insert(meta->url, { job, { meta } });

    const auto full_entry_path = entry->getFullPath();
    const auto source_url = meta->url;
    const auto generation = m_generation;
    auto loadImage = [this, doc, generation, source_url](const QImage& image, const QString& animatedPath) {
        // If we flushed, don't proceed.
        if (generation != m_generation)
            return;

        const auto targets = m_pending.take(source_url).targets;
        if (targets.isEmpty())
            return;

        doc->addResource(QTextDocument::ImageResource, source_url, image);

        for (const auto& target : targets) {
            target->image = image;
            parseImage(doc, target);
        }

        // pickle-descriptions: a GIF or animated WebP plays instead of showing its first frame
        if (!animatedPath.isEmpty())
            startAnimation(source_url.toDisplayString(), animatedPath);

        // This size hack is needed to prevent the content from being laid out in an area smaller
        // than the total width available (weird).
        auto size = doc->pageSize();
        doc->adjustSize();
        doc->setPageSize(size);
    };
    connect(job, &NetJob::succeeded, this, [full_entry_path, loadImage] {
        qDebug() << "Loaded resource at:" << full_entry_path;
        loadImage(QImage(full_entry_path), full_entry_path);
    });
    connect(job, &NetJob::failed, this, [full_entry_path, loadImage](QString reason) {
        qWarning() << "Failed resource at:" << full_entry_path << "because:" << reason;
        loadImage(QImage(), QString());
    });
    connect(job, &NetJob::finished, job, &NetJob::deleteLater);

    job->start();
}
