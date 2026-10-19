// SPDX-License-Identifier: GPL-3.0-only

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QTest>

#include "sharedpack/SharedPackSigV4.h"
#include "sharedpack/SharedPackTransfer.h"

using namespace SharedPack;

namespace {

QString shaOf(const QByteArray& data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha512).toHex());
}

Invite s3Invite()
{
    Invite invite;
    invite.packId = "pack-1";
    invite.endpoint = "https://acct.r2.cloudflarestorage.com";
    invite.bucket = "friends-packs";
    invite.region = "auto";
    invite.token = "AbCdEfGhIjKlMnOpQrStUv";
    invite.accessKeyId = "0123456789abcdef0123456789abcdef";
    invite.secretAccessKey = "fedcba9876543210fedcba9876543210fedcba98";
    return invite;
}

Invite publicInvite()
{
    Invite invite;
    invite.packId = "pack-1";
    invite.endpoint = "https://files.example.com/share";
    invite.token = "AbCdEfGhIjKlMnOpQrStUv";
    return invite;
}

Limiter::Options options(qint64 expected, const QByteArray& content = {})
{
    Limiter::Options o;
    o.allowedHosts = { "files.example.com" };
    o.maxBytes = 1000;
    o.expectedSize = expected;
    if (!content.isEmpty())
        o.expectedSha512 = shaOf(content);
    return o;
}

}  // namespace

class SharedPackTransferTest : public QObject {
    Q_OBJECT

   private slots:
    // ---- Limiter: where ----
    void urlMustBeHttpsOnAllowedHost()
    {
        Limiter limiter(options(-1));
        QString why;
        QVERIFY(limiter.checkUrl(QUrl("https://files.example.com/a"), &why));
        QVERIFY(limiter.checkUrl(QUrl("https://FILES.example.com/a"), &why));  // host names are case-insensitive
        QVERIFY(!limiter.checkUrl(QUrl("http://files.example.com/a"), &why));
        QVERIFY(!limiter.checkUrl(QUrl("https://evil.example.org/a"), &why));  // a redirect somewhere else
        QVERIFY(!limiter.checkUrl(QUrl("https://files.example.com.evil.org/a"), &why));
        QVERIFY(!limiter.checkUrl(QUrl("https://files.example.com:8443/a"), &why));  // another port is another service
        QVERIFY(!limiter.checkUrl(QUrl("https://user:pw@files.example.com/a"), &why));
        QVERIFY(!limiter.checkUrl(QUrl("ftp://files.example.com/a"), &why));
        QVERIFY(!limiter.checkUrl(QUrl(), &why));
    }

    void httpOnlyWhenTestsAskForIt()
    {
        auto o = options(-1);
        o.requireHttps = false;
        Limiter limiter(o);
        QVERIFY(limiter.checkUrl(QUrl("http://files.example.com/a"), nullptr));
        QVERIFY(!limiter.checkUrl(QUrl("ftp://files.example.com/a"), nullptr));
    }

    // ---- Limiter: how much ----
    void exactDownloadPasses()
    {
        const QByteArray content(300, 'x');
        Limiter limiter(options(300, content));
        QVERIFY(limiter.add(content.left(100), nullptr));
        QVERIFY(limiter.add(content.mid(100), nullptr));
        QString why;
        QVERIFY2(limiter.finish(200, &why), qPrintable(why));
        QCOMPARE(limiter.received(), qint64(300));
    }

    void biggerThanDeclaredStopsWhileStreaming()
    {
        Limiter limiter(options(100));
        QVERIFY(limiter.add(QByteArray(100, 'x'), nullptr));
        QString why;
        QVERIFY(!limiter.add(QByteArray(1, 'x'), &why));  // the 101st byte
        QVERIFY(!limiter.add(QByteArray(), nullptr));     // and nothing more is accepted
        QVERIFY(!limiter.finish(200, nullptr));
    }

    void maxBytesIsTheCapWhenTheSizeIsUnknown()
    {
        Limiter limiter(options(-1));
        QVERIFY(limiter.add(QByteArray(1000, 'x'), nullptr));
        QVERIFY(!limiter.add(QByteArray(1, 'x'), nullptr));
    }

    void smallerThanDeclaredFails()
    {
        Limiter limiter(options(100));
        QVERIFY(limiter.add(QByteArray(99, 'x'), nullptr));
        QVERIFY(!limiter.finish(200, nullptr));
    }

    void wrongContentFails()
    {
        const QByteArray expected(100, 'x');
        Limiter limiter(options(100, expected));
        QVERIFY(limiter.add(QByteArray(100, 'y'), nullptr));  // same size, other bytes
        QString why;
        QVERIFY(!limiter.finish(200, &why));
        QVERIFY2(why.contains("checksum"), qPrintable(why));
    }

    void onlyStatus200Counts()
    {
        const QByteArray content(10, 'x');
        for (int status : { 0, 204, 206, 301, 304, 403, 404, 500 }) {
            Limiter limiter(options(10, content));
            QVERIFY(limiter.add(content, nullptr));
            QVERIFY2(!limiter.finish(status, nullptr), qPrintable(QString::number(status)));
        }
    }

    void resetStartsAnAttemptAgain()
    {
        const QByteArray content(10, 'x');
        Limiter limiter(options(10, content));
        QVERIFY(limiter.add(content, nullptr));
        QVERIFY(!limiter.add(QByteArray(1, 'x'), nullptr));  // failed attempt
        limiter.reset();
        QVERIFY(limiter.add(content, nullptr));
        QVERIFY(limiter.finish(200, nullptr));
    }

    void reasonsNeverContainTheInput()
    {
        Limiter limiter(options(10));
        QString why;
        QVERIFY(!limiter.checkUrl(QUrl("https://evil.example.org/secret-token-path"), &why));
        QVERIFY2(!why.contains("secret-token-path") && !why.contains("evil"), qPrintable(why));
    }

    // ---- sources ----
    void sourcesAreModrinthFirstThenTheBlob()
    {
        File file;
        file.path = "mods/a.jar";
        file.size = 10;
        file.sha512 = QString(128, QLatin1Char('a'));
        file.urls = { "https://cdn.modrinth.com/data/AANobbMI/versions/x/a.jar", "https://evil.example.org/a.jar",
                      "http://cdn.modrinth.com/data/a" };
        const QList<Source> sources = sourcesFor(s3Invite(), file);
        QCOMPARE(sources.size(), 2);  // the two addresses a manifest may not have are dropped
        QVERIFY(!sources[0].isBlob);
        QCOMPARE(sources[0].url.host(), QStringLiteral("cdn.modrinth.com"));
        QVERIFY(sources[1].isBlob);
        QCOMPARE(sources[1].url.toString(),
                 "https://acct.r2.cloudflarestorage.com/friends-packs/" + s3Invite().token + "/files/" + file.sha512);
    }

    void sourcesWithoutUrlsAreJustTheBlob()
    {
        File file;
        file.sha512 = QString(128, QLatin1Char('b'));
        QCOMPARE(sourcesFor(publicInvite(), file).size(), 1);
    }

    // ---- signing ----
    void s3RequestsAreSigned()
    {
        const Invite invite = s3Invite();
        const QDateTime now(QDate(2026, 10, 15), QTime(12, 30, 45), Qt::UTC);
        const QUrl url = invite.urlFor("latest.json");
        const HeaderList headers = signedGetHeaders(invite, url, now);
        QCOMPARE(headers.size(), 3);

        QHash<QByteArray, QByteArray> byName;
        for (const auto& header : headers)
            byName.insert(header.first, header.second);
        QCOMPARE(byName.value("x-amz-date"), QByteArray("20261015T123045Z"));
        QCOMPARE(byName.value("x-amz-content-sha256"), SigV4::emptyPayloadHash().toLatin1());

        const QString auth = QString::fromLatin1(byName.value("Authorization"));
        const QRegularExpression shape(
            "^AWS4-HMAC-SHA256 Credential=0123456789abcdef0123456789abcdef/20261015/auto/s3/aws4_request,"
            "SignedHeaders=host;x-amz-content-sha256;x-amz-date,Signature=[0-9a-f]{64}$");
        QVERIFY2(shape.match(auth).hasMatch(), qPrintable(auth));
        QVERIFY(!auth.contains(invite.secretAccessKey));  // the secret itself never goes on the wire

        // the same request signed by hand gives the same signature: the wiring (host, path, region) is right
        SigV4::Request request;
        request.method = "GET";
        request.path = "/friends-packs/" + invite.token + "/latest.json";
        request.headers.insert("host", "acct.r2.cloudflarestorage.com");
        request.payloadHash = SigV4::emptyPayloadHash();
        request.time = now;
        request.region = "auto";
        QCOMPARE(SigV4::sign({ invite.accessKeyId, invite.secretAccessKey }, request).authorization, auth);
    }

    void publicInvitesAreNeverSigned()
    {
        const Invite invite = publicInvite();
        QVERIFY(signedGetHeaders(invite, invite.urlFor("latest.json"), QDateTime::currentDateTimeUtc()).isEmpty());
    }

    void keysAreNotSentToOtherHosts()
    {
        const Invite invite = s3Invite();
        const QDateTime now = QDateTime::currentDateTimeUtc();
        QVERIFY(signedGetHeaders(invite, QUrl("https://cdn.modrinth.com/data/a/versions/b/c.jar"), now).isEmpty());
        QVERIFY(signedGetHeaders(invite, QUrl("https://acct.r2.cloudflarestorage.com.evil.org/x"), now).isEmpty());
    }

    void differentUrlsGetDifferentSignatures()
    {
        const Invite invite = s3Invite();
        const QDateTime now = QDateTime::currentDateTimeUtc();
        const auto a = signedGetHeaders(invite, invite.urlFor("latest.json"), now);
        const auto b = signedGetHeaders(invite, invite.urlFor("revisions/2.json"), now);
        QVERIFY(a.last().second != b.last().second);
    }
};

QTEST_GUILESS_MAIN(SharedPackTransferTest)

#include "SharedPackTransfer_test.moc"
