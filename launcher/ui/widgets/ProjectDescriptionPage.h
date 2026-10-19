#pragma once

#include <QTextBrowser>

#include "QObjectPtr.h"

QT_BEGIN_NAMESPACE
class VariableSizedImageObject;
QT_END_NAMESPACE

/** This subclasses QTextBrowser to provide additional capabilities
 *  to it, like allowing for images to be shown.
 */
class ProjectDescriptionPage final : public QTextBrowser {
    Q_OBJECT

   public:
    ProjectDescriptionPage(QWidget* parent = nullptr);

    void setMetaEntry(QString entry);

    /** Like QTextBrowser::setHtml, but makes CurseForge's relative outgoing links work (pickle-descriptions). */
    void setHtml(const QString& html);

   public slots:
    /** Flushes the current processing happening in the page.
     *
     *  Should be called when changing the page's content entirely, to
     *  prevent old tasks from changing the new content.
     */
    void flush();

   protected:
    // pickle-descriptions: animated images only play while the page is shown
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

   private:
    shared_qobject_ptr<VariableSizedImageObject> m_image_text_object;
    bool m_flushDone = false;  // pickle-descriptions: setHtml flushed already, the flush() that follows it is absorbed
};
