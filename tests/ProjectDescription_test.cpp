// SPDX-License-Identifier: GPL-3.0-only

#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QTemporaryFile>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <memory>

#include "ui/widgets/ProjectDescriptionUtils.h"
#include "ui/widgets/VariableSizedImageObject.h"

using namespace ProjectDescription;

namespace {

// The button row of EntityCulling as Modrinth serves it (after markdown), shortened.
const QString kModrinthButtons =
    "<p align=\"center\" style=\"text-align: center;\">\n"
    "  <a href=\"https://discord.gg/caVV5eXekm\"><img src=\"https://x.test/Discord.png\" alt=\"Discord\" style=\"margin: 5px 10px;\"></a>\n"
    "  <a href=\"https://github.com/tr7zw/EntityCulling\"><img src=\"https://x.test/Github.png\" alt=\"GitHub\"></a>\n"
    "  <a href=\"https://modrinth.com/mod/entityculling\"><img src=\"https://x.test/Modrinth.png\" alt=\"GitHub\"></a>\n"
    "  <a href=\"https://www.curseforge.com/minecraft/mc-mods/entityculling\"><img src=\"https://x.test/Curseforge.png\" "
    "alt=\"GitHub\"></a>\n"
    "</p>";

// The same row as CurseForge serves it: two outgoing links are relative "linkout" addresses.
const QString kCurseForgeButtons =
    "<p style=\"text-align:center\">\n"
    "  <a href=\"https://discord.gg/caVV5eXekm\" rel=\"nofollow\"><img src=\"https://x.test/Discord.png\"></a>\n"
    "  <a href=\"/linkout?remoteUrl=https%253a%252f%252fmodrinth.com%252fmod%252fentityculling\" rel=\"nofollow\"><img "
    "src=\"https://x.test/Modrinth.png\"></a>\n"
    "  <a href=\"/linkout?remoteUrl=http%253a%252f%252fessential.gg%252f\" target=\"_blank\" rel=\"nofollow\"><img "
    "src=\"https://x.test/Essential.png\"></a>\n"
    "</p>";

// A 1x1 GIF; with `frames` > 1 it is an animation (two frames, looping).
QByteArray tinyGif(int frames)
{
    QByteArray gif = QByteArray::fromHex(
        "474946383961"
        "0100"
        "0100"
        "80"
        "00"
        "00"
        "ffffff"
        "000000");
    if (frames > 1)
        gif += QByteArray::fromHex("21ff0b4e45545343415045322e300301000000");
    for (int i = 0; i < frames; i++)
        gif += QByteArray::fromHex(
            "21f90404"
            "0a00"
            "0000"
            "2c00000000"
            "0100"
            "0100"
            "00"
            "0202440100");
    gif += QByteArray::fromHex("3b");
    return gif;
}

// Writes the data to a file without an extension, like the meta cache does. The file lives as long as the object.
std::unique_ptr<QTemporaryFile> cachedFile(const QByteArray& data)
{
    auto file = std::make_unique<QTemporaryFile>(QDir::tempPath() + "/pdtestXXXXXX");
    if (!file->open() || file->write(data) != data.size())
        return nullptr;
    file->flush();
    return file;
}

QList<int> imagePositions(QTextDocument& doc)
{
    QList<int> positions;
    for (QTextBlock block = doc.begin(); block.isValid(); block = block.next())
        for (auto it = block.begin(); !it.atEnd(); ++it)
            if (it.fragment().charFormat().isImageFormat())
                positions << it.fragment().position();
    return positions;
}

}  // namespace

class ProjectDescriptionTest : public QObject {
    Q_OBJECT

   private slots:
    void charFormatAt_keepsTheLinkOfEveryImage()
    {
        QTextDocument doc;
        doc.setHtml(kModrinthButtons);

        const auto positions = imagePositions(doc);
        QCOMPARE(positions.size(), 4);

        const QStringList links{ "https://discord.gg/caVV5eXekm", "https://github.com/tr7zw/EntityCulling",
                                 "https://modrinth.com/mod/entityculling", "https://www.curseforge.com/minecraft/mc-mods/entityculling" };
        for (int i = 0; i < positions.size(); i++) {
            const auto format = charFormatAt(&doc, positions[i]);
            QVERIFY(format.isImageFormat());
            QCOMPARE(format.anchorHref(), links[i]);
        }
    }

    void charFormatAt_aCollapsedCursorLosesTheLink()
    {
        // Documents why charFormatAt exists: the plain cursor gives the link of the first image only.
        QTextDocument doc;
        doc.setHtml(kModrinthButtons);

        const auto positions = imagePositions(doc);
        QCOMPARE(positions.size(), 4);

        QTextCursor first(&doc);
        first.setPosition(positions[0]);
        QVERIFY(!first.charFormat().anchorHref().isEmpty());

        QTextCursor second(&doc);
        second.setPosition(positions[1]);
        QVERIFY(second.charFormat().anchorHref().isEmpty());
    }

    void resolveLinkouts_replacesBothEncodings()
    {
        const auto html = resolveLinkouts(kCurseForgeButtons);
        QVERIFY(html.contains("href=\"https://modrinth.com/mod/entityculling\""));
        QVERIFY(html.contains("href=\"http://essential.gg/\""));
        QVERIFY(!html.contains("linkout"));
        // everything around the links stays as it was
        QVERIFY(html.contains("href=\"https://discord.gg/caVV5eXekm\" rel=\"nofollow\""));
        QVERIFY(html.contains("target=\"_blank\" rel=\"nofollow\""));
    }

    void resolveLinkouts_keepsQueryParametersEscaped()
    {
        // https://x.test/?a=1&b=2, encoded twice; the & must be an entity inside the attribute
        const auto html = resolveLinkouts("<a href=\"/linkout?remoteUrl=https%253a%252f%252fx.test%252f%253fa%253d1%2526b%253d2\">x</a>");
        QCOMPARE(html, QString("<a href=\"https://x.test/?a=1&amp;b=2\">x</a>"));
    }

    void resolveLinkouts_singleQuotesAndExtraParameters()
    {
        const auto html = resolveLinkouts("<a href='/linkout?remoteUrl=https%253a%252f%252fx.test%252f&foo=1'>x</a>");
        QCOMPARE(html, QString("<a href='https://x.test/'>x</a>"));
    }

    void resolveLinkouts_leavesOtherLinksAlone()
    {
        const QStringList untouched{
            "<a href=\"https://x.test/linkout?remoteUrl=https%253a%252f%252fy.test\">x</a>",  // absolute, not CurseForge's relative form
            "<a href=\"/other?remoteUrl=https%253a%252f%252fy.test\">x</a>",
            "<a href=\"/linkout?remoteUrl=javascript%253aalert(1)\">x</a>",  // not http(s)
            "<a href=\"/linkout?remoteUrl=file%253a%252f%252f%252fC%253a%252fx\">x</a>",
            "<a href=\"/linkout?remoteUrl=\">x</a>",
            "no links at all",
        };
        for (const auto& html : untouched)
            QCOMPARE(resolveLinkouts(html), html);
    }

    void imageRefs_listsEveryImageEvenWhenNeighboursAreEqual()
    {
        // the first two images have the same format, so the document keeps them in one text fragment
        QTextDocument doc;
        doc.setHtml(
            "<p><img src=\"https://x.test/a.png\" width=\"10\" height=\"20\"><img src=\"https://x.test/a.png\" width=\"10\" "
            "height=\"20\"> text <img src=\"https://x.test/b.png\"></p>");

        const auto refs = imageRefs(&doc);
        QCOMPARE(refs.size(), 3);

        QCOMPARE(refs[0].url, QUrl("https://x.test/a.png"));
        QCOMPARE(refs[1].url, QUrl("https://x.test/a.png"));
        QVERIFY(refs[0].position != refs[1].position);
        QCOMPARE(refs[0].width, 10);
        QCOMPARE(refs[0].height, 20);

        QCOMPARE(refs[2].url, QUrl("https://x.test/b.png"));
        QCOMPARE(refs[2].width, 0);
        QCOMPARE(refs[2].height, 0);
        QVERIFY(refs[2].position > refs[1].position);

        // every position really is an image character
        for (const auto& ref : refs)
            QVERIFY(charFormatAt(&doc, ref.position).isImageFormat());
    }

    void imageRefs_noImagesNoRefs()
    {
        QTextDocument doc;
        doc.setHtml("<p>nothing <b>to</b> see</p><table><tr><td>cell</td></tr></table>");
        QVERIFY(imageRefs(&doc).isEmpty());
    }

    void imageRefs_findsImagesInTables()
    {
        QTextDocument doc;
        doc.setHtml("<table><tr><td><img src=\"https://x.test/a.png\"></td><td><img src=\"https://x.test/b.png\"></td></tr></table>");
        QCOMPARE(imageRefs(&doc).size(), 2);
    }

    void isAnimatedImage_detectsFramesFromTheContent()
    {
        if (!QImageReader::supportedImageFormats().contains("gif"))
            QSKIP("no GIF plugin");

        const auto animated = cachedFile(tinyGif(2));
        QVERIFY(animated);
        QVERIFY(QFileInfo(animated->fileName()).suffix().isEmpty());  // no extension to go by
        QVERIFY(isAnimatedImage(animated->fileName()));

        const auto still = cachedFile(tinyGif(1));
        QVERIFY(still);
        QVERIFY(!isAnimatedImage(still->fileName()));
    }

    void isAnimatedImage_hugeFramesStayStill()
    {
        if (!QImageReader::supportedImageFormats().contains("gif"))
            QSKIP("no GIF plugin");

        const auto animated = cachedFile(tinyGif(2));
        QVERIFY(animated);
        QVERIFY(isAnimatedImage(animated->fileName(), 1));
        QVERIFY(!isAnimatedImage(animated->fileName(), 0));  // the one pixel is already over the limit
    }

    void isAnimatedImage_notAnImage()
    {
        const auto text = cachedFile("this is not an image");
        QVERIFY(text);
        QVERIFY(!isAnimatedImage(text->fileName()));
        QVERIFY(!isAnimatedImage("this/file/does/not/exist"));
    }

    void intrinsicSize_reservesTheRoomBeforeTheImageIsLoaded()
    {
        QTextDocument doc;
        doc.setTextWidth(800);
        VariableSizedImageObject object;

        QTextImageFormat both;
        both.setName("https://x.test/a.png");
        both.setWidth(300);
        both.setHeight(100);
        QCOMPARE(object.intrinsicSize(&doc, 0, both), QSizeF(300, 100));

        // wider than the page: scaled down, keeping the proportions
        QTextImageFormat wide = both;
        wide.setWidth(2000);
        wide.setHeight(1000);
        const auto available = 800 - 2 * doc.documentMargin();
        QCOMPARE(object.intrinsicSize(&doc, 0, wide), QSizeF(available, available / 2));
    }

    void intrinsicSize_noRoomWithoutBothDimensions()
    {
        QTextDocument doc;
        doc.setTextWidth(800);
        VariableSizedImageObject object;

        QTextImageFormat none;
        none.setName("https://x.test/a.png");
        QVERIFY(object.intrinsicSize(&doc, 0, none).isEmpty());

        // the height follows from the image, which is not there yet
        QTextImageFormat onlyWidth = none;
        onlyWidth.setWidth(300);
        QVERIFY(object.intrinsicSize(&doc, 0, onlyWidth).isEmpty());
    }
};

int main(int argc, char** argv)
{
    // QTextDocument needs a GUI application object (fonts); no display is needed.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    ProjectDescriptionTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "ProjectDescription_test.moc"
