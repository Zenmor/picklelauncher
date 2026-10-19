
#include "ProjectDescriptionPage.h"

#include "ProjectDescriptionUtils.h"
#include "VariableSizedImageObject.h"

#include <QDebug>
#include <utility>

ProjectDescriptionPage::ProjectDescriptionPage(QWidget* parent) : QTextBrowser(parent), m_image_text_object(new VariableSizedImageObject)
{
    m_image_text_object->setParent(this);
    document()->documentLayout()->registerHandler(QTextFormat::ImageObject, m_image_text_object.get());
    // pickle-descriptions: a new frame of an animated image is only a repaint, not a new layout
    connect(m_image_text_object.get(), &VariableSizedImageObject::animationFrame, viewport(), qOverload<>(&QWidget::update));
}

void ProjectDescriptionPage::setMetaEntry(QString entry)
{
    if (m_image_text_object)
        m_image_text_object->setMetaEntry(entry);
}

void ProjectDescriptionPage::setHtml(const QString& html)
{
    // pickle-descriptions: the old content's downloads end here, the new content's images start downloading right away
    if (m_image_text_object)
        m_image_text_object->flush();
    QTextBrowser::setHtml(ProjectDescription::resolveLinkouts(html));
    if (m_image_text_object)
        m_image_text_object->preload(document());
    m_flushDone = true;
}

void ProjectDescriptionPage::showEvent(QShowEvent* event)
{
    QTextBrowser::showEvent(event);
    if (m_image_text_object)
        m_image_text_object->setSuspended(false);
    viewport()->update();  // the animations go on when their images are painted
}

void ProjectDescriptionPage::hideEvent(QHideEvent* event)
{
    QTextBrowser::hideEvent(event);
    if (m_image_text_object)
        m_image_text_object->setSuspended(true);
}

void ProjectDescriptionPage::flush()
{
    // pickle-descriptions: the pages call this right after setHtml, which has already done it (and started the
    // downloads of the new content, which this must not cancel)
    if (std::exchange(m_flushDone, false))
        return;

    if (m_image_text_object)
        m_image_text_object->flush();
}
