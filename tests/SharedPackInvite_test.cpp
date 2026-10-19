// SPDX-License-Identifier: GPL-3.0-only

#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

#include "sharedpack/SharedPackInvite.h"
#include "sharedpack/SharedPackRedact.h"

using namespace SharedPack;

namespace {

const QString Token = QStringLiteral("AbCdEfGhIjKlMnOpQrStUv");

QString encode(const QJsonObject& object, const QString& prefix = QStringLiteral("picklepack:1:"))
{
    return prefix + QString::fromLatin1(QJsonDocument(object)
                                            .toJson(QJsonDocument::Compact)
                                            .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QJsonObject publicObject()
{
    return QJsonObject{ { "packId", "pack-1" }, { "endpoint", "https://files.example.com/share" }, { "token", Token } };
}

QJsonObject s3Object()
{
    return QJsonObject{ { "packId", "pack-1" },
                        { "name", "Friends Pack" },
                        { "endpoint", "https://acct.r2.cloudflarestorage.com" },
                        { "bucket", "friends-packs" },
                        { "prefix", "a/b" },
                        { "region", "auto" },
                        { "token", Token },
                        { "accessKeyId", "0123456789abcdef0123456789abcdef" },
                        { "secretAccessKey", "fedcba9876543210fedcba9876543210fedcba98" } };
}

QJsonObject without(QJsonObject object, const char* key)
{
    object.remove(QLatin1String(key));
    return object;
}

QJsonObject with(QJsonObject object, const char* key, const QJsonValue& value)
{
    object.insert(QLatin1String(key), value);
    return object;
}

}  // namespace

class SharedPackInviteTest : public QObject {
    Q_OBJECT

   private slots:
    void publicMode()
    {
        const auto parsed = parseInvite(encode(publicObject()));
        QVERIFY2(parsed, qPrintable(parsed.error));
        QCOMPARE(parsed.value->mode(), Invite::Mode::Public);
        QCOMPARE(parsed.value->packId, QStringLiteral("pack-1"));
        QCOMPARE(parsed.value->endpoint, QStringLiteral("https://files.example.com/share"));
        QCOMPARE(parsed.value->urlFor("latest.json").toString(), "https://files.example.com/share/" + Token + "/latest.json");
        QCOMPARE(parsed.value->host(), QStringLiteral("files.example.com"));
    }

    void s3Mode()
    {
        const auto parsed = parseInvite(encode(s3Object()));
        QVERIFY2(parsed, qPrintable(parsed.error));
        QCOMPARE(parsed.value->mode(), Invite::Mode::S3);
        // path-style: the bucket is in the path, and the signed path is the path of the URL
        QCOMPARE(parsed.value->signedPathFor("revisions/3.json"), "/friends-packs/a/b/" + Token + "/revisions/3.json");
        QCOMPARE(parsed.value->urlFor("revisions/3.json").toString(),
                 "https://acct.r2.cloudflarestorage.com/friends-packs/a/b/" + Token + "/revisions/3.json");
        QCOMPARE(parsed.value->secrets().size(), 3);
    }

    void s3RegionDefaultsToAuto()
    {
        const auto parsed = parseInvite(encode(without(s3Object(), "region")));
        QVERIFY(parsed);
        QCOMPARE(parsed.value->region, QStringLiteral("auto"));
    }

    void roundTrip()
    {
        for (const QJsonObject& object : { publicObject(), s3Object() }) {
            const auto first = parseInvite(encode(object));
            QVERIFY(first);
            const QString text = inviteToString(*first.value);
            QVERIFY(text.startsWith("picklepack:1:"));
            const auto second = parseInvite(text);
            QVERIFY2(second, qPrintable(second.error));
            QCOMPARE(inviteToString(*second.value), text);
            QCOMPARE(second.value->urlFor("latest.json"), first.value->urlFor("latest.json"));
        }
    }

    void base64Variants()
    {
        // every length mod 3 (the name changes the length), with and without padding
        for (int extra = 0; extra < 4; ++extra) {
            const QJsonObject object = with(publicObject(), "name", QString(extra, QLatin1Char('x')));
            const QByteArray json = QJsonDocument(object).toJson(QJsonDocument::Compact);
            const QString unpadded =
                "picklepack:1:" + QString::fromLatin1(json.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
            const QString padded = "picklepack:1:" + QString::fromLatin1(json.toBase64(QByteArray::Base64UrlEncoding));
            QVERIFY2(parseInvite(unpadded), qPrintable(unpadded));
            QVERIFY2(parseInvite(padded), qPrintable(padded));
        }
    }

    void surroundingWhitespaceIsIgnored() { QVERIFY(parseInvite("  \n" + encode(publicObject()) + " \r\n")); }

    void tooNew()
    {
        const auto parsed = parseInvite(encode(publicObject(), "picklepack:3:"));
        QVERIFY(!parsed);
        QCOMPARE(parsed.failure, Failure::TooNew);
    }

    void inviteV2MultiEndpoints()
    {
        QJsonObject obj;
        obj.insert("packId", "pack-1");
        obj.insert("token", Token);
        obj.insert("name", "V2 Pack");
        obj.insert("prefix", "packs");
        QJsonArray eps;
        eps.append("https://packs.zenmor.xyz");
        eps.append("https://pub-123456789.r2.dev");
        obj.insert("endpoints", eps);

        const QString encoded = encode(obj, "picklepack:2:");
        const auto parsed = parseInvite(encoded);
        QVERIFY2(parsed, qPrintable(parsed.error));
        QCOMPARE(parsed.value->version, 2);
        QCOMPARE(parsed.value->mode(), Invite::Mode::Public);
        QCOMPARE(parsed.value->endpoints.size(), 2);
        QCOMPARE(parsed.value->urlFor("latest.json", 0).toString(), "https://packs.zenmor.xyz/packs/" + Token + "/latest.json");
        QCOMPARE(parsed.value->urlFor("latest.json", 1).toString(), "https://pub-123456789.r2.dev/packs/" + Token + "/latest.json");
        QCOMPARE(parsed.value->host(0), QStringLiteral("packs.zenmor.xyz"));
        QCOMPARE(parsed.value->host(1), QStringLiteral("pub-123456789.r2.dev"));

        // Roundtrip
        const QString out = inviteToString(*parsed.value);
        QVERIFY(out.startsWith("picklepack:2:"));
        const auto parsed2 = parseInvite(out);
        QVERIFY(parsed2);
        QCOMPARE(parsed2.value->endpoints, parsed.value->endpoints);

        // v2 with keys rejected
        obj.insert("accessKeyId", "0123456789abcdef0123456789abcdef");
        const auto bad = parseInvite(encode(obj, "picklepack:2:"));
        QVERIFY(!bad);
    }

    void garbage_data()
    {
        QTest::addColumn<QString>("text");
        const QString good = encode(publicObject());
        QTest::newRow("empty") << QString();
        QTest::newRow("words") << QStringLiteral("hello there");
        QTest::newRow("scheme only") << QStringLiteral("picklepack:");
        QTest::newRow("no version") << QStringLiteral("picklepack::abc");
        QTest::newRow("version 0") << encode(publicObject(), "picklepack:0:");
        QTest::newRow("version text") << encode(publicObject(), "picklepack:x:");
        QTest::newRow("upper scheme") << good.toUpper();
        QTest::newRow("bad base64") << QStringLiteral("picklepack:1:!!!!");
        QTest::newRow("not json") << QStringLiteral("picklepack:1:") +
                                         QString::fromLatin1(QByteArray("hello").toBase64(QByteArray::Base64UrlEncoding));
        QTest::newRow("json array") << QStringLiteral("picklepack:1:") +
                                           QString::fromLatin1(QByteArray("[1]").toBase64(QByteArray::Base64UrlEncoding));
        QTest::newRow("too long") << good + QString(MaxInviteChars, QLatin1Char('A'));
        QTest::newRow("http") << encode(with(publicObject(), "endpoint", "http://files.example.com"));
        QTest::newRow("ftp") << encode(with(publicObject(), "endpoint", "ftp://files.example.com"));
        QTest::newRow("no endpoint") << encode(without(publicObject(), "endpoint"));
        QTest::newRow("credentials in url") << encode(with(publicObject(), "endpoint", "https://user:pw@files.example.com"));
        QTest::newRow("query in url") << encode(with(publicObject(), "endpoint", "https://files.example.com/?a=b"));
        QTest::newRow("fragment in url") << encode(with(publicObject(), "endpoint", "https://files.example.com/#x"));
        QTest::newRow("dotdot in url") << encode(with(publicObject(), "endpoint", "https://files.example.com/a/../b"));
        QTest::newRow("unicode host") << encode(with(publicObject(), "endpoint", QStringLiteral("https://f\u00fcles.example.com")));
        QTest::newRow("endpoint not string") << encode(with(publicObject(), "endpoint", 5));
        QTest::newRow("no token") << encode(without(publicObject(), "token"));
        QTest::newRow("short token") << encode(with(publicObject(), "token", "abc"));
        QTest::newRow("token with slash") << encode(with(publicObject(), "token", "AbCdEfGhIjKlMnOpQr/Stu"));
        QTest::newRow("token with dot") << encode(with(publicObject(), "token", "AbCdEfGhIjKlMnOpQr..tu"));
        QTest::newRow("no packId") << encode(without(publicObject(), "packId"));
        QTest::newRow("bad packId") << encode(with(publicObject(), "packId", "../x"));
        QTest::newRow("prefix dotdot") << encode(with(publicObject(), "prefix", "a/../b"));
        QTest::newRow("prefix slash") << encode(with(publicObject(), "prefix", "/a"));
        QTest::newRow("public with secret") << encode(with(publicObject(), "secretAccessKey", "fedcba9876543210fedcba98"));
        QTest::newRow("public with bucket") << encode(with(publicObject(), "bucket", "friends-packs"));
        QTest::newRow("s3 no secret") << encode(without(s3Object(), "secretAccessKey"));
        QTest::newRow("s3 no bucket") << encode(without(s3Object(), "bucket"));
        QTest::newRow("s3 bad bucket") << encode(with(s3Object(), "bucket", "Bad_Bucket"));
        QTest::newRow("s3 endpoint with path") << encode(with(s3Object(), "endpoint", "https://acct.r2.cloudflarestorage.com/x"));
        QTest::newRow("s3 short key") << encode(with(s3Object(), "accessKeyId", "abc"));
        QTest::newRow("s3 key with space") << encode(with(s3Object(), "accessKeyId", "0123456789 abcdef"));
    }

    void garbage()
    {
        QFETCH(QString, text);
        const auto parsed = parseInvite(text);
        QVERIFY(!parsed);
        // the error text is for the log: it must never repeat the input
        QVERIFY2(!parsed.error.contains(Token), qPrintable(parsed.error));
        QVERIFY2(parsed.error.size() < 80, qPrintable(parsed.error));
    }

    void extractFindsTheInviteInText()
    {
        const QString invite = encode(publicObject());
        QCOMPARE(extractInvite(invite), invite);
        QCOMPARE(extractInvite("  " + invite + "\n"), invite);
        QCOMPARE(extractInvite("Here you go: " + invite + " enjoy the pack!"), invite);
        QCOMPARE(extractInvite("\"" + invite + "\"."), invite);
        QCOMPARE(extractInvite("<" + invite + ">, thanks"), invite);
        QCOMPARE(extractInvite(encode(s3Object()) + " and more"), encode(s3Object()));
    }

    void extractJoinsABrokenInvite()
    {
        const QString invite = encode(s3Object());
        QString wrapped;
        for (qsizetype i = 0, line = 0; i < invite.size(); i += 40, ++line)
            wrapped += invite.mid(i, 40) + (line % 2 ? "\r\n" : "\n  ");
        QVERIFY(wrapped.contains(QLatin1Char('\n')));
        QCOMPARE(extractInvite("Invite:\n" + wrapped + "\nSee you there!"), invite);
    }

    void extractTakesTheFirstOfTwoInvites()
    {
        const QString first = encode(publicObject());
        const QString second = encode(with(publicObject(), "name", "Other"));
        QVERIFY(first != second);
        QCOMPARE(extractInvite(first + "\n" + second), first);
        QCOMPARE(extractInvite(second + " or " + first), second);
        // A broken first one does not hide a good second one.
        QCOMPARE(extractInvite("picklepack:1:AAAA and then " + first), first);
    }

    void extractRefusesGarbage()
    {
        for (const QString& text : QStringList{ QString(), "   ", "no invite here", "picklepack:", "picklepack:1:", "picklepack:1:AAAA",
                                                "picklepack:1:AAAA BBBB CCCC",
                                                "picklepack:2:" + encode(with(publicObject(), "accessKeyId", "bad")).mid(11),
                                                encode(publicObject()).left(40), "PICKLEPACK:1:abc" })
            QVERIFY2(extractInvite(text).isEmpty(), qPrintable(text));
        // Whatever it returns, the parser accepts: it never selects something the strict rules refuse.
        QVERIFY(extractInvite(QString(100000, QLatin1Char('a')) + encode(publicObject())).isEmpty());  // bounded work
        const QString bad = encode(with(s3Object(), "bucket", "Bad_Bucket"));
        QVERIFY(extractInvite(bad).isEmpty());
    }

    void errorsNeverContainSecrets()
    {
        const auto parsed = parseInvite(encode(with(s3Object(), "bucket", "Bad_Bucket")));
        QVERIFY(!parsed);
        for (const QString& secret : QStringList{ Token, "0123456789abcdef0123456789abcdef", "fedcba9876543210" })
            QVERIFY(!parsed.error.contains(secret));
    }

    void redactorCoversUrls()
    {
        const auto parsed = parseInvite(encode(s3Object()));
        QVERIFY(parsed);
        Redactor redactor;
        for (const QString& secret : parsed.value->secrets())
            redactor.add(secret);
        const QString line = "Requesting " + parsed.value->urlFor("latest.json").toString() + " key=" + parsed.value->accessKeyId;
        const QString clean = redactor.apply(line);
        QVERIFY2(!clean.contains(Token), qPrintable(clean));
        QVERIFY2(!clean.contains(parsed.value->accessKeyId), qPrintable(clean));
        QVERIFY2(clean.contains("[redacted]"), qPrintable(clean));
    }

    void tokenRules()
    {
        QVERIFY(isValidToken(Token));
        QVERIFY(!isValidToken(Token.left(21)));
        QVERIFY(!isValidToken(QString(129, QLatin1Char('a'))));
        QVERIFY(!isValidToken(Token + "/"));
        QVERIFY(!isValidToken(Token + QStringLiteral("\u00e9")));
    }
};

QTEST_GUILESS_MAIN(SharedPackInviteTest)

#include "SharedPackInvite_test.moc"
