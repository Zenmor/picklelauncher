// SPDX-License-Identifier: GPL-3.0-only

#include <QGuiApplication>
#include <QJsonDocument>
#include <QTest>
#include <QTextDocument>

#include "modplatform/PackGallery.h"
#include "ui/widgets/ProjectDescriptionUtils.h"

using namespace ModPlatform;

namespace {

// JSON with ' instead of ": the samples hold URLs, and moc cannot read raw strings that contain //
QJsonArray array(QByteArray json)
{
    return QJsonDocument::fromJson(json.replace('\'', '"')).array();
}

// Shaped like GET /v2/project/{id} of Modrinth
const char* kModrinthGallery =
    "["
    "{'url':'https://cdn.modrinth.com/data/x/images/b_350.webp','raw_url':'https://cdn.modrinth.com/data/x/images/b.webp',"
    " 'featured':false,'title':'Second','description':'d','created':'2024-01-01T00:00:00Z','ordering':1},"
    "{'url':'https://cdn.modrinth.com/data/x/images/a_350.webp','raw_url':'https://cdn.modrinth.com/data/x/images/a.webp',"
    " 'featured':false,'title':'First','description':null,'created':'2024-01-01T00:00:00Z','ordering':0},"
    "{'url':'https://cdn.modrinth.com/data/x/images/f_350.webp','raw_url':'https://cdn.modrinth.com/data/x/images/f.webp',"
    " 'featured':true,'title':null,'ordering':5},"
    "{'url':'javascript:alert','raw_url':'javascript:alert','featured':true,'ordering':-1},"
    "{'title':'no address at all'}"
    "]";

// Shaped like the "screenshots" of a CurseForge mod
const char* kCurseForgeScreenshots =
    "["
    "{'id':1,'modId':9,'title':'Default Image','description':'',"
    " 'thumbnailUrl':'https://media.forgecdn.net/attachments/thumbnails/10/571/310/172/banner.png',"
    " 'url':'https://media.forgecdn.net/attachments/10/571/banner.png'},"
    "{'id':2,'modId':9,'title':'The spawn','description':'',"
    " 'thumbnailUrl':'https://media.forgecdn.net/attachments/thumbnails/1/2/310/172/spawn.png',"
    " 'url':'https://media.forgecdn.net/attachments/1/2/spawn.png'},"
    "{'id':3,'title':'no thumbnail','thumbnailUrl':'','url':'https://media.forgecdn.net/attachments/3/4/x.png'},"
    "{'id':4,'title':'broken','thumbnailUrl':'https://media.forgecdn.net/t.png','url':''}"
    "]";
}  // namespace

class PackGalleryTest : public QObject {
    Q_OBJECT

   private slots:
    void modrinth_featuredFirstThenOrdering()
    {
        const auto images = parseModrinthGallery(array(kModrinthGallery));
        QCOMPARE(images.size(), 3);

        QCOMPARE(images[0].url, QString("https://cdn.modrinth.com/data/x/images/f.webp"));
        QCOMPARE(images[1].title, QString("First"));
        QCOMPARE(images[2].title, QString("Second"));
    }

    void modrinth_thumbnailIsTheSmallOneAndTheLinkTheOriginal()
    {
        const auto images = parseModrinthGallery(array(kModrinthGallery));
        QVERIFY(!images.isEmpty());
        QCOMPARE(images[1].thumbnailUrl, QString("https://cdn.modrinth.com/data/x/images/a_350.webp"));
        QCOMPARE(images[1].url, QString("https://cdn.modrinth.com/data/x/images/a.webp"));
        QVERIFY(images[0].title.isEmpty());  // null title
    }

    void modrinth_oneAddressIsEnough()
    {
        const auto images = parseModrinthGallery(array("[{'url':'https://x.test/a.png'},{'raw_url':'https://x.test/b.png'}]"));
        QCOMPARE(images.size(), 2);
        QCOMPARE(images[0].thumbnailUrl, QString("https://x.test/a.png"));
        QCOMPARE(images[0].url, QString("https://x.test/a.png"));
        QCOMPARE(images[1].thumbnailUrl, QString("https://x.test/b.png"));
        QCOMPARE(images[1].url, QString("https://x.test/b.png"));
    }

    void emptyAndWrongShapes()
    {
        QVERIFY(parseModrinthGallery({}).isEmpty());
        QVERIFY(parseCurseForgeScreenshots({}).isEmpty());
        QVERIFY(parseModrinthGallery(array("['a', 1, null, []]")).isEmpty());
        QVERIFY(parseCurseForgeScreenshots(array("['a', 1, null, []]")).isEmpty());
        QVERIFY(galleryHtml({}).isEmpty());
    }

    void curseForge_thumbnailFallsBackToTheImage_brokenEntriesAreDropped()
    {
        const auto images = parseCurseForgeScreenshots(array(kCurseForgeScreenshots));
        QCOMPARE(images.size(), 3);

        QCOMPARE(images[1].thumbnailUrl, QString("https://media.forgecdn.net/attachments/thumbnails/1/2/310/172/spawn.png"));
        QCOMPARE(images[1].url, QString("https://media.forgecdn.net/attachments/1/2/spawn.png"));

        QCOMPARE(images[2].thumbnailUrl, QString("https://media.forgecdn.net/attachments/3/4/x.png"));
        QCOMPARE(images[2].url, QString("https://media.forgecdn.net/attachments/3/4/x.png"));
    }

    void html_headingThumbnailsAndLinks()
    {
        const auto html = galleryHtml(parseCurseForgeScreenshots(array(kCurseForgeScreenshots)));
        QVERIFY(html.contains("<h3>Gallery</h3>"));
        QVERIFY(
            html.contains("<a href=\"https://media.forgecdn.net/attachments/1/2/spawn.png\">"
                          "<img src=\"https://media.forgecdn.net/attachments/thumbnails/1/2/310/172/spawn.png\"></a>"));
        QVERIFY(html.contains("The spawn"));
    }

    void html_titlesThatAreOnlyFileNamesAreNotShown()
    {
        const QList<GalleryImage> images{
            { "https://x.test/t1.png", "https://x.test/1.png", "Default Image" },
            { "https://x.test/t2.png", "https://x.test/2.png", "image-attachment_69545b4f-0e02.PNG" },
            { "https://x.test/t3.png", "https://x.test/3.png", "  " },
            { "https://x.test/t4.png", "https://x.test/4.png", "Night view" },
        };
        const auto html = galleryHtml(images);
        QVERIFY(!html.contains("Default Image"));
        QVERIFY(!html.contains("image-attachment"));
        QVERIFY(html.contains("<br>Night view</p>"));
        QCOMPARE(html.count("<br>"), 1);
    }

    void html_escapesEverything()
    {
        const QList<GalleryImage> images{ { "https://x.test/t.png?a=1&b=\"2\"", "https://x.test/f.png?a=1&b=2",
                                            "<b>Tom & \"Jerry\"</b>" } };
        const auto html = galleryHtml(images);
        QVERIFY(html.contains("href=\"https://x.test/f.png?a=1&amp;b=2\""));
        QVERIFY(html.contains("src=\"https://x.test/t.png?a=1&amp;b=&quot;2&quot;\""));
        QVERIFY(html.contains("&lt;b&gt;Tom &amp; &quot;Jerry&quot;&lt;/b&gt;"));
        QVERIFY(!html.contains("<b>"));
    }

    void html_atMostTheGivenNumberOfImages()
    {
        QList<GalleryImage> images;
        for (int i = 0; i < 25; i++)
            images.append({ QString("https://x.test/t%1.png").arg(i), QString("https://x.test/%1.png").arg(i), {} });

        QCOMPARE(galleryHtml(images).count("<img "), 10);
        QCOMPARE(galleryHtml(images, 3).count("<img "), 3);
        QCOMPARE(galleryHtml(images, 100).count("<img "), 25);
        QVERIFY(galleryHtml(images, 0).isEmpty());
        QVERIFY(galleryHtml(images, 3).contains("t2.png"));
        QVERIFY(!galleryHtml(images, 3).contains("t3.png"));
    }

    void html_importsAsLinkedImages()
    {
        // what the description page does with it: every thumbnail is an image that links to the full picture
        QTextDocument doc;
        doc.setHtml(galleryHtml(parseModrinthGallery(array(kModrinthGallery))));

        const auto refs = ProjectDescription::imageRefs(&doc);
        QCOMPARE(refs.size(), 3);
        QCOMPARE(refs[1].url, QUrl("https://cdn.modrinth.com/data/x/images/a_350.webp"));

        const auto format = ProjectDescription::charFormatAt(&doc, refs[1].position);
        QCOMPARE(format.anchorHref(), QString("https://cdn.modrinth.com/data/x/images/a.webp"));
    }
};

int main(int argc, char** argv)
{
    // QTextDocument needs a GUI application object (fonts); no display is needed.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    PackGalleryTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "PackGallery_test.moc"
