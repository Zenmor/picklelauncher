// SPDX-License-Identifier: GPL-3.0-only

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFileInfo>
#include <QTest>
#include <QTimer>
#include <functional>
#include <optional>
#include <memory>

#include "sharedpack/SharedPackApply.h"
#include "sharedpack/SharedPackBuild.h"
#include "sharedpack/SharedPackDisk.h"
#include "sharedpack/SharedPackLink.h"
#include "sharedpack/SharedPackPublishPlan.h"
#include "sharedpack/SharedPackPublisher.h"
#include "sharedpack/SharedPackRelayTarget.h"
#include "sharedpack/SharedPackSigV4.h"
#include "sharedpack/SharedPackTarget.h"
#include "sharedpack/SharedPackTransfer.h"

using namespace SharedPack;

Q_DECLARE_METATYPE(SharedPack::UploadPlan)

namespace {

QString sha512Of(const QByteArray& data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha512).toHex());
}
QString sha256Of(const QByteArray& data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

HostConfig s3Config()
{
    HostConfig config;
    config.kind = HostConfig::Kind::S3;
    config.endpoint = "https://0123456789abcdef.r2.cloudflarestorage.com";
    config.bucket = "mypacks";
    config.prefix = "shared";
    config.region = "auto";
    config.writeKeyId = "WRITEKEYID1234567890";
    config.writeSecret = "WRITESECRETvalue0123456789abcdefghijklmn";
    config.readKeyId = "READKEYID01234567890";
    config.readSecret = "READSECRETvalue0123456789abcdefghijklmno";
    return config;
}

HostConfig folderConfig(const QString& folder)
{
    HostConfig config;
    config.kind = HostConfig::Kind::Folder;
    config.folder = folder;
    config.endpoint = "https://files.example.com/share";
    return config;
}

QString header(const S3Request& request, const QByteArray& name)
{
    for (const auto& pair : request.headers)
        if (pair.first.toLower() == name.toLower())
            return QString::fromLatin1(pair.second);
    return {};
}

/// A folder target that refuses chosen operations: how the tests make an upload fail halfway.
class GatedTarget : public FolderTarget {
   public:
    using FolderTarget::FolderTarget;
    std::function<std::optional<int>(const Op&)> failWith;  ///< an HTTP status to answer instead of doing the operation
    int refused = 0;
    int released = 0;  ///< how often the publisher gave the host's session back

    void releaseSession() override { ++released; }

    void run(const Op& op, Done done, Progress progress = {}) override
    {
        if (failWith) {
            if (const auto status = failWith(op)) {
                ++refused;
                OpResult result;
                result.status = *status;
                result.error = QStringLiteral("refused by the test (HTTP %1)").arg(*status);
                QTimer::singleShot(0, this, [done, result] { done(result); });
                return;
            }
        }
        FolderTarget::run(op, std::move(done), std::move(progress));
    }
};

struct Fixture {
    QTemporaryDir game;
    QTemporaryDir host;
    QByteArray modA = QByteArray(3000, 'A');
    QByteArray modPublic = QByteArray(2000, 'P');
    QByteArray config = "key=value\n";
    QByteArray options = "fov:90\n";
    BuildInput input;

    Fixture()
    {
        writeFile(game.path() + "/mods/a.jar", modA);
        writeFile(game.path() + "/mods/pub.jar", modPublic);
        writeFile(game.path() + "/config/x.toml", config);
        writeFile(game.path() + "/options.txt", options);
        writeFile(game.path() + "/saves/w/level.dat", "world");
        input.packId = "pack-1";
        input.name = "Friends Pack";
        input.changelog = "first";
        input.components = { { "net.minecraft", "1.21.1" }, { "net.fabricmc.fabric-loader", "0.16.5" } };
        input.files = { file("mods/a.jar", modA), file("mods/pub.jar", modPublic, true), file("config/x.toml", config), file("options.txt", options),
                        file("saves/w/level.dat", "world") };
    }

    File file(const QString& path, const QByteArray& data, bool publicUrl = false) const
    {
        File f{ path, data.size(), sha512Of(data), {} };
        if (publicUrl)
            f.urls = { QStringLiteral("https://cdn.modrinth.com/data/AABBCCDD/versions/11223344/") + QFileInfo(path).fileName() };
        return f;
    }

    PublishJob job(int revision, const QString& token, std::optional<int> baseline)
    {
        input.revision = revision;
        const BuildResult built = buildManifest(input);
        PublishJob result;
        result.manifest = *built.manifest;
        result.token = token;
        result.baseline = baseline;
        for (const File& f : result.manifest.files)
            if (needsBlob(f)) {
                const QString path = game.path() + QLatin1Char('/') + f.path;
                result.blobs.insert(f.sha512, LocalBlob{ path, f.size, sha256Of(readFile(path)) });
            }
        return result;
    }

    QString packDir(const QString& token) const { return host.path() + "/" + token; }
};

constexpr const char* TokenA = "AAAAAAAAAAAAAAAAAAAAAA";
constexpr const char* TokenB = "BBBBBBBBBBBBBBBBBBBBBB";

/// Runs one publish to the end and returns {ok, error}.
std::pair<bool, QString> publishAndWait(Publisher& publisher, const PublishJob& job, bool dryRun = false)
{
    QSignalSpy spy(&publisher, &Publisher::finished);
    publisher.publish(job, dryRun);
    if (spy.isEmpty() && !spy.wait(15000))
        return { false, "timeout" };
    return { spy.first().at(0).toBool(), spy.first().at(1).toString() };
}

}  // namespace

class SharedPackTargetTest : public QObject {
    Q_OBJECT

   private slots:
    void initTestCase()
    {
        qRegisterMetaType<SharedPack::UploadPlan>();
        Publisher::setRetryDelayScale(0);  // the retry tests below should not sit through the real pauses
    }

    void cleanupTestCase() { Publisher::setRetryDelayScale(1); }

    // ---------------------------------------------------------------- keys
    void keys()
    {
        QVERIFY(isValidKey("shared/AbC_-123456789012345/latest.json"));
        QVERIFY(isValidKey("TOKEN/files/" + QString(128, 'a')));
        QVERIFY(!isValidKey(""));
        QVERIFY(!isValidKey("/abs/latest.json"));
        QVERIFY(!isValidKey("a//b"));
        QVERIFY(!isValidKey("a/../b"));
        QVERIFY(!isValidKey("a\\b"));
        QVERIFY(!isValidKey("a b/c"));
        QVERIFY(!isValidKey("a/b?x=1"));
        QVERIFY(!isValidKey("a/con"));  // Windows device name: the folder target runs on Windows
        QVERIFY(!isValidKey("a/b."));
    }

    // ---------------------------------------------------------------- S3 requests (no network)
    void s3PutDocument()
    {
        const QDateTime now(QDate(2026, 10, 15), QTime(12, 0, 0), QTimeZone::UTC);
        Op op;
        op.kind = Op::Kind::Put;
        op.key = "shared/TOKEN12345678901234567/latest.json";
        op.body = "{\"formatVersion\":1}";
        const S3Request request = buildS3Request(s3Config(), op, now);
        QVERIFY(request.valid);
        QCOMPARE(request.method, QStringLiteral("PUT"));
        QCOMPARE(request.url.toString(), QStringLiteral("https://0123456789abcdef.r2.cloudflarestorage.com/mypacks/shared/TOKEN12345678901234567/latest.json"));
        QCOMPARE(header(request, "x-amz-content-sha256"), sha256Of(op.body));
        QCOMPARE(header(request, "x-amz-date"), QStringLiteral("20261015T120000Z"));
        QCOMPARE(header(request, "Content-Type"), QStringLiteral("application/json"));

        // The signature is the one the existing signer (checked against the AWS vectors) makes for exactly this request.
        SigV4::Request expected;
        expected.method = "PUT";
        expected.path = "/mypacks/shared/TOKEN12345678901234567/latest.json";
        expected.headers = { { "host", "0123456789abcdef.r2.cloudflarestorage.com" }, { "content-type", "application/json" } };
        expected.payloadHash = sha256Of(op.body);
        expected.time = now;
        expected.region = "auto";
        const auto signature = SigV4::sign({ "WRITEKEYID1234567890", s3Config().writeSecret }, expected);
        QCOMPARE(header(request, "Authorization"), signature.authorization);
        QVERIFY(signature.authorization.startsWith("AWS4-HMAC-SHA256 Credential=WRITEKEYID1234567890/20261015/auto/s3/aws4_request"));
        QVERIFY(signature.authorization.contains("SignedHeaders=content-type;host;x-amz-content-sha256;x-amz-date"));
    }

    void s3PutFileUsesTheRealPayloadHash()
    {
        const QDateTime now(QDate(2026, 10, 15), QTime(12, 0, 0), QTimeZone::UTC);
        Op op;
        op.kind = Op::Kind::Put;
        op.key = "shared/TOKEN12345678901234567/files/" + QString(128, 'a');
        op.file = "C:/x/a.jar";
        op.size = 10;
        QVERIFY(!buildS3Request(s3Config(), op, now).valid);  // no payload hash: refused, never "UNSIGNED-PAYLOAD"
        op.sha256 = QString(64, 'b');
        const S3Request request = buildS3Request(s3Config(), op, now);
        QVERIFY(request.valid);
        QCOMPARE(header(request, "x-amz-content-sha256"), QString(64, 'b'));
        QCOMPARE(header(request, "Content-Type"), QStringLiteral("application/octet-stream"));
    }

    void s3OtherVerbs()
    {
        const QDateTime now(QDate(2026, 10, 15), QTime(12, 0, 0), QTimeZone::UTC);
        Op op;
        op.key = "shared/TOKEN12345678901234567/latest.json";
        const std::pair<Op::Kind, const char*> verbs[] = { { Op::Kind::Head, "HEAD" }, { Op::Kind::Get, "GET" }, { Op::Kind::Delete, "DELETE" } };
        for (const auto& verb : verbs) {
            op.kind = verb.first;
            const S3Request request = buildS3Request(s3Config(), op, now);
            QVERIFY(request.valid);
            QCOMPARE(request.method, QString(verb.second));
            QCOMPARE(header(request, "x-amz-content-sha256"), SigV4::emptyPayloadHash());
        }
        op.kind = Op::Kind::Copy;
        op.copyFrom = "shared/OLDTOKEN1234567890123/latest.json";
        const S3Request copy = buildS3Request(s3Config(), op, now);
        QVERIFY(copy.valid);
        QCOMPARE(copy.method, QStringLiteral("PUT"));
        QCOMPARE(header(copy, "x-amz-copy-source"), QStringLiteral("/mypacks/shared/OLDTOKEN1234567890123/latest.json"));
        QVERIFY(copy.headers.size() == 4);
        // The copy header is part of what is signed, otherwise the host refuses the request.
        QVERIFY(header(copy, "Authorization").contains("x-amz-copy-source"));
    }

    void s3RefusesWhatItCannotSendSafely()
    {
        const QDateTime now = QDateTime::currentDateTimeUtc();
        Op op;
        op.kind = Op::Kind::Head;
        op.key = "shared/TOKEN12345678901234567/latest.json";
        HostConfig config = s3Config();
        config.endpoint = "http://0123456789abcdef.r2.cloudflarestorage.com";  // https only
        QVERIFY(!buildS3Request(config, op, now).valid);
        QVERIFY(!buildS3Request(folderConfig("C:/x"), op, now).valid);
        op.key = "../x";
        QVERIFY(!buildS3Request(s3Config(), op, now).valid);
        op.key = "a/b";
        op.kind = Op::Kind::Copy;
        op.copyFrom = "a/../b";
        QVERIFY(!buildS3Request(s3Config(), op, now).valid);
    }

    void s3NeverUsesTheReadKey()
    {
        Op op;
        op.kind = Op::Kind::Delete;
        op.key = "shared/TOKEN12345678901234567/latest.json";
        const S3Request request = buildS3Request(s3Config(), op, QDateTime::currentDateTimeUtc());
        const QString authorization = header(request, "Authorization");
        QVERIFY(authorization.contains("WRITEKEYID1234567890"));
        QVERIFY(!authorization.contains("READKEYID"));
        QVERIFY(!authorization.contains(s3Config().writeSecret));  // the secret is only ever an input of the signature
        for (const auto& pair : request.headers)
            QVERIFY(!pair.second.contains(s3Config().writeSecret.toLatin1()));
        QVERIFY(!request.url.toString().contains("WRITE"));
    }

    // ---------------------------------------------------------------- folder target
    void folderOps()
    {
        QTemporaryDir dir;
        FolderTarget target(dir.path());
        Op put;
        put.kind = Op::Kind::Put;
        put.key = "p/TOKEN/latest.json";
        put.body = "{}";
        QVERIFY(target.runSync(put).ok);
        Op head;
        head.kind = Op::Kind::Head;
        head.key = put.key;
        QVERIFY(target.runSync(head).ok);
        head.key = "p/TOKEN/nothing";
        QVERIFY(target.runSync(head).absent());
        Op get;
        get.kind = Op::Kind::Get;
        get.key = put.key;
        QCOMPARE(target.runSync(get).body, QByteArray("{}"));

        Op copy;
        copy.kind = Op::Kind::Copy;
        copy.key = "p/NEW/latest.json";
        copy.copyFrom = put.key;
        QVERIFY(target.runSync(copy).ok);
        QCOMPARE(readFile(dir.path() + "/p/NEW/latest.json"), QByteArray("{}"));
        copy.copyFrom = "p/TOKEN/missing";
        QVERIFY(target.runSync(copy).absent());

        Op del;
        del.kind = Op::Kind::Delete;
        del.key = put.key;
        QVERIFY(target.runSync(del).ok);
        QVERIFY(target.runSync(del).ok);  // deleting what is not there is fine, like on S3
        QVERIFY(!QDir(dir.path() + "/p/TOKEN").exists());  // the emptied folder went with it
        QVERIFY(QDir(dir.path() + "/p/NEW").exists());

        put.key = "../escape";
        QVERIFY(!target.runSync(put).ok);
        QVERIFY(!QFile::exists(QFileInfo(dir.path()).absolutePath() + "/escape"));
    }

    void folderChecksTheBlobWhileCopying()
    {
        QTemporaryDir dir;
        QTemporaryDir src;
        writeFile(src.path() + "/a.bin", "hello");
        FolderTarget target(dir.path());
        Op put;
        put.kind = Op::Kind::Put;
        put.key = "t/files/x";
        put.file = src.path() + "/a.bin";
        put.size = 5;
        put.sha512 = sha512Of("hello");
        QVERIFY(target.runSync(put).ok);
        put.key = "t/files/y";
        put.sha512 = sha512Of("other");  // the file is not what was hashed
        QVERIFY(!target.runSync(put).ok);
        QVERIFY(!QFile::exists(dir.path() + "/t/files/y"));  // and nothing, not even a temporary file, is left
        QCOMPARE(QDir(dir.path() + "/t/files").entryList(QDir::Files), QStringList{ "x" });
        put.sha512 = sha512Of("hello");
        put.size = 6;
        QVERIFY(!target.runSync(put).ok);
    }

    // ---------------------------------------------------------------- publish to a folder, read back with the member side
    void publishToAFolderEndToEnd()
    {
        Fixture f;
        FolderTarget target(f.host.path());
        Publisher publisher(&target, folderConfig(f.host.path()));

        const PublishJob job = f.job(1, TokenA, std::nullopt);
        QSignalSpy planned(&publisher, &Publisher::planned);
        const auto [ok, error] = publishAndWait(publisher, job);
        QVERIFY2(ok, qPrintable(error));
        QCOMPARE(planned.size(), 1);

        // Only files that need one have a blob: not the public jar, not saves.
        const QString dir = f.packDir(TokenA);
        QStringList blobs = QDir(dir + "/files").entryList(QDir::Files);
        blobs.sort();
        QStringList wanted{ sha512Of(f.modA), sha512Of(f.config), sha512Of(f.options) };
        wanted.sort();
        QCOMPARE(blobs, wanted);
        QVERIFY(!QFile::exists(dir + "/files/" + sha512Of(f.modPublic)));
        for (const QString& sha : blobs)
            QCOMPARE(sha512Of(readFile(dir + "/files/" + sha)), sha);  // content is what its name says

        // What a member reads.
        const auto latest = parseLatest(readFile(dir + "/latest.json"));
        QVERIFY(latest);
        QCOMPARE(latest.value->revision, 1);
        QCOMPARE(latest.value->packId, QStringLiteral("pack-1"));
        const auto manifest = parseManifest(readFile(dir + "/revisions/1.json"));
        QVERIFY(manifest);
        QCOMPARE(manifest.value->files.size(), 4);  // saves/ is not in it

        // Phase 1 logic on a member's empty game folder: it sees an update, plans four writes, finds every blob it needs on the host
        // (the public jar comes from its Modrinth URL first), and applying the downloaded blobs reproduces the owner's files.
        State member;
        member.packId = "pack-1";
        QCOMPARE(checkForUpdate(member, *latest.value), UpdateCheck::Available);

        QTemporaryDir memberDir;
        const Layout layout = layoutFor(memberDir.path(), memberDir.path() + "/minecraft");
        QVERIFY(QDir().mkpath(layout.gameDir));  // an instance always has its game folder
        QStringList paths;
        for (const File& file : manifest.value->files)
            paths.append(file.path);
        const Plan plan = planUpdate(member, *manifest.value, scanDisk(layout.gameDir, paths));
        QVERIFY(plan);
        QCOMPARE(plan.actions.size(), 4);

        Invite invite;
        invite.packId = "pack-1";
        invite.endpoint = "https://files.example.com/share";
        invite.token = TokenA;
        for (const File& file : manifest.value->files) {
            const auto sources = sourcesFor(invite, file);
            if (needsBlob(file)) {
                QCOMPARE(sources.size(), 1);
                QVERIFY(sources.first().isBlob);
                QVERIFY(QFile::exists(dir + "/files/" + file.sha512));
                QVERIFY(writeFile(layout.blobPath(file.sha512), readFile(dir + "/files/" + file.sha512)));
            } else {
                QCOMPARE(sources.size(), 2);  // the public URL, then the pack's own (unused) blob
                QVERIFY(!sources.first().isBlob);
                QVERIFY(writeFile(layout.blobPath(file.sha512), f.modPublic));  // as if downloaded from the CDN
            }
        }
        const ApplyResult applied = applyPlan(layout, member, plan);
        QVERIFY2(applied.ok, qPrintable(applied.error));
        QCOMPARE(readFile(layout.gameDir + "/mods/a.jar"), f.modA);
        QCOMPARE(readFile(layout.gameDir + "/mods/pub.jar"), f.modPublic);
        QCOMPARE(readFile(layout.gameDir + "/config/x.toml"), f.config);
        QCOMPARE(readFile(layout.gameDir + "/options.txt"), f.options);
        QVERIFY(!QFile::exists(layout.gameDir + "/saves/w/level.dat"));
    }

    void secondPublishOnlyUploadsWhatChanged()
    {
        Fixture f;
        FolderTarget target(f.host.path());
        Publisher publisher(&target, folderConfig(f.host.path()));
        QVERIFY(publishAndWait(publisher, f.job(1, TokenA, std::nullopt)).first);

        // Same files again as revision 2: every blob is on the host, so the plan is the revision and latest.json.
        QSignalSpy planned(&publisher, &Publisher::planned);
        const auto [ok, error] = publishAndWait(publisher, f.job(2, TokenA, 1));
        QVERIFY2(ok, qPrintable(error));
        const auto plan = planned.first().at(0).value<UploadPlan>();
        QCOMPARE(plan.blobCount, 0);
        QCOMPARE(plan.puts.size(), 2);
        QCOMPARE(parseLatest(readFile(f.packDir(TokenA) + "/latest.json")).value->revision, 2);
        QVERIFY(QFile::exists(f.packDir(TokenA) + "/revisions/1.json"));  // history stays: a rollback republishes old content as revision 3
        QVERIFY(QFile::exists(f.packDir(TokenA) + "/revisions/2.json"));

        // Changing one file uploads exactly one new blob.
        writeFile(f.game.path() + "/config/x.toml", "key=changed\n");
        f.config = "key=changed\n";
        f.input.files[2] = f.file("config/x.toml", f.config);
        QSignalSpy planned3(&publisher, &Publisher::planned);
        QVERIFY(publishAndWait(publisher, f.job(3, TokenA, 2)).first);
        QCOMPARE(planned3.first().at(0).value<UploadPlan>().blobCount, 1);
    }

    void dryRunChangesNothing()
    {
        Fixture f;
        FolderTarget target(f.host.path());
        Publisher publisher(&target, folderConfig(f.host.path()));
        QSignalSpy planned(&publisher, &Publisher::planned);
        QVERIFY(publishAndWait(publisher, f.job(1, TokenA, std::nullopt), true).first);
        QCOMPARE(planned.first().at(0).value<UploadPlan>().blobCount, 3);
        QVERIFY(QDir(f.host.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }

    void failedBlobNeverSwitchesOver()
    {
        Fixture f;
        // Revision 1 is live.
        {
            FolderTarget target(f.host.path());
            Publisher publisher(&target, folderConfig(f.host.path()));
            QVERIFY(publishAndWait(publisher, f.job(1, TokenA, std::nullopt)).first);
        }
        const QByteArray liveLatest = readFile(f.packDir(TokenA) + "/latest.json");

        // Revision 2 has a new blob that cannot be uploaded.
        writeFile(f.game.path() + "/config/x.toml", "key=changed\n");
        f.config = "key=changed\n";
        f.input.files[2] = f.file("config/x.toml", f.config);
        GatedTarget target(f.host.path());
        const QString badBlob = "files/" + sha512Of(f.config);
        target.failWith = [badBlob](const Op& op) -> std::optional<int> {
            return op.kind == Op::Kind::Put && op.key.endsWith(badBlob) ? std::optional<int>(403) : std::nullopt;
        };
        Publisher publisher(&target, folderConfig(f.host.path()));
        const auto [ok, error] = publishAndWait(publisher, f.job(2, TokenA, 1));
        QVERIFY(!ok);
        QVERIFY(error.contains("403"));
        QCOMPARE(readFile(f.packDir(TokenA) + "/latest.json"), liveLatest);  // still revision 1, fully there
        QVERIFY(!QFile::exists(f.packDir(TokenA) + "/revisions/2.json"));     // not even the revision was written

        // After the problem is gone, the same publish finishes (resumable: it skips what is already on the host).
        target.failWith = nullptr;
        QVERIFY(publishAndWait(publisher, f.job(2, TokenA, 1)).first);
        QCOMPARE(parseLatest(readFile(f.packDir(TokenA) + "/latest.json")).value->revision, 2);
    }

    void failedRevisionNeverSwitchesOver()
    {
        Fixture f;
        GatedTarget target(f.host.path());
        target.failWith = [](const Op& op) -> std::optional<int> {
            return op.kind == Op::Kind::Put && op.key.contains("revisions/") ? std::optional<int>(403) : std::nullopt;
        };
        Publisher publisher(&target, folderConfig(f.host.path()));
        QVERIFY(!publishAndWait(publisher, f.job(1, TokenA, std::nullopt)).first);
        QVERIFY(!QFile::exists(f.packDir(TokenA) + "/latest.json"));  // a first publish that failed leaves no pack at all
    }

    void abortOnAFolderStopsBeforeTheNextFile()
    {
        // A folder copy cannot be interrupted and nothing is in flight between two files, so abortAll() alone changes nothing there: the
        // publisher itself must refuse to start anything after the abort.
        Fixture f;
        {
            FolderTarget target(f.host.path());
            Publisher publisher(&target, folderConfig(f.host.path()));
            QVERIFY(publishAndWait(publisher, f.job(1, TokenA, std::nullopt)).first);
        }
        const QByteArray liveLatest = readFile(f.packDir(TokenA) + "/latest.json");

        // Revision 2 changes three blobs.
        f.modA = QByteArray(3000, 'B');
        f.config = "key=changed\n";
        f.options = "fov:70\n";
        writeFile(f.game.path() + "/mods/a.jar", f.modA);
        writeFile(f.game.path() + "/config/x.toml", f.config);
        writeFile(f.game.path() + "/options.txt", f.options);
        f.input.files[0] = f.file("mods/a.jar", f.modA);
        f.input.files[2] = f.file("config/x.toml", f.config);
        f.input.files[3] = f.file("options.txt", f.options);

        GatedTarget target(f.host.path());
        Publisher publisher(&target, folderConfig(f.host.path()));
        int blobsStarted = 0;
        target.failWith = [&](const Op& op) -> std::optional<int> {
            if (op.kind == Op::Kind::Put && op.key.contains("/files/") && ++blobsStarted == 1)
                publisher.abort();  // Cancel pressed while the first file is being copied
            return std::nullopt;    // the operation itself still runs
        };
        const auto [ok, error] = publishAndWait(publisher, f.job(2, TokenA, 1));
        QVERIFY(!ok);
        QCOMPARE(error, QStringLiteral("Aborted."));
        QCOMPARE(blobsStarted, 1);                                           // the other two blobs never started
        QVERIFY(!QFile::exists(f.packDir(TokenA) + "/revisions/2.json"));  // neither did the revision
        QCOMPARE(readFile(f.packDir(TokenA) + "/latest.json"), liveLatest);  // latest.json still says revision 1

        // Nothing is stuck: the same publish goes through afterwards.
        target.failWith = nullptr;
        QVERIFY(publishAndWait(publisher, f.job(2, TokenA, 1)).first);
        QCOMPARE(parseLatest(readFile(f.packDir(TokenA) + "/latest.json")).value->revision, 2);
    }

    void networkErrorsAreRetried()
    {
        Fixture f;
        GatedTarget target(f.host.path());
        int failures = 0;
        target.failWith = [&failures](const Op& op) -> std::optional<int> {
            if (op.kind == Op::Kind::Put && op.key.contains("files/") && failures < 2) {
                ++failures;
                return 503;
            }
            return std::nullopt;
        };
        Publisher publisher(&target, folderConfig(f.host.path()));
        const auto [ok, error] = publishAndWait(publisher, f.job(1, TokenA, std::nullopt));
        QVERIFY2(ok, qPrintable(error));
        QCOMPARE(failures, 2);

        // A refusal (4xx) is not retried.
        Fixture g;
        GatedTarget refusing(g.host.path());
        refusing.failWith = [](const Op& op) -> std::optional<int> {
            return op.kind == Op::Kind::Put && op.key.contains("files/") ? std::optional<int>(403) : std::nullopt;
        };
        Publisher second(&refusing, folderConfig(g.host.path()));
        QVERIFY(!publishAndWait(second, g.job(1, TokenA, std::nullopt)).first);
        QVERIFY(refusing.refused <= Publisher::MaxConcurrentPuts);  // each in-flight blob was tried once
    }

    void failedPublishGivesTheHostSessionBack()
    {
        // A Shared Packs Server keeps the pack reserved until the session expires, so a publish that dies must let go of it.
        Fixture f;
        GatedTarget target(f.host.path());
        target.failWith = [](const Op& op) -> std::optional<int> {
            return op.kind == Op::Kind::Put && op.key.contains("files/") ? std::optional<int>(403) : std::nullopt;
        };
        Publisher publisher(&target, folderConfig(f.host.path()));
        QVERIFY(!publishAndWait(publisher, f.job(1, TokenA, std::nullopt)).first);
        QCOMPARE(target.released, 1);

        target.failWith = nullptr;
        QVERIFY(publishAndWait(publisher, f.job(1, TokenA, std::nullopt)).first);
        QCOMPARE(target.released, 1);  // a publish that worked has nothing to give back
    }

    void commitIsRetriedAndAConflictIsNot()
    {
        // 500 and 429 on the final step are tried again (the Worker accepts a repeated commit of the same session); 409 is final.
        const auto isLatest = [](const Op& op) { return op.kind == Op::Kind::Put && op.key.endsWith("latest.json"); };
        for (const int transient : { 500, 429 }) {
            Fixture f;
            GatedTarget target(f.host.path());
            target.failWith = [&target, isLatest, transient](const Op& op) -> std::optional<int> {
                return isLatest(op) && target.refused < 2 ? std::optional<int>(transient) : std::nullopt;
            };
            Publisher publisher(&target, folderConfig(f.host.path()));
            const auto [ok, error] = publishAndWait(publisher, f.job(1, TokenA, std::nullopt));
            QVERIFY2(ok, qPrintable(error));
            QCOMPARE(target.refused, 2);
            QCOMPARE(target.released, 0);
        }

        Fixture g;
        GatedTarget conflicting(g.host.path());
        conflicting.failWith = [isLatest](const Op& op) -> std::optional<int> { return isLatest(op) ? std::optional<int>(409) : std::nullopt; };
        Publisher second(&conflicting, folderConfig(g.host.path()));
        QVERIFY(!publishAndWait(second, g.job(1, TokenA, std::nullopt)).first);
        QCOMPARE(conflicting.refused, 1);
        QCOMPARE(conflicting.released, 1);
    }

    void retriesWaitBeforeTryingAgain()
    {
        Publisher::setRetryDelayScale(1);
        // 5xx and no answer: short pauses. 429: pauses that together outlast the Worker's one-minute window.
        QCOMPARE(Publisher::retryDelayMs(503, 0), 1000);
        QCOMPARE(Publisher::retryDelayMs(503, 1), 3000);
        QCOMPARE(Publisher::retryDelayMs(0, 0), 1000);
        QCOMPARE(Publisher::retryDelayMs(429, 0), 20000);
        QCOMPARE(Publisher::retryDelayMs(429, 1), 40000);
        QVERIFY(Publisher::retryDelayMs(429, 0) + Publisher::retryDelayMs(429, 1) >= 60000);

        // A retry really waits: with a 20 ms pause (scale 0.02 of 1 s) two failed attempts cost at least 20 ms + 60 ms.
        Publisher::setRetryDelayScale(0.02);
        Fixture f;
        GatedTarget target(f.host.path());
        const auto isLatest = [](const Op& op) { return op.kind == Op::Kind::Put && op.key.endsWith("latest.json"); };
        target.failWith = [&target, isLatest](const Op& op) -> std::optional<int> {
            return isLatest(op) && target.refused < 2 ? std::optional<int>(503) : std::nullopt;
        };
        Publisher publisher(&target, folderConfig(f.host.path()));
        QElapsedTimer clock;
        clock.start();
        QVERIFY(publishAndWait(publisher, f.job(1, TokenA, std::nullopt)).first);
        QCOMPARE(target.refused, 2);
        QVERIFY2(clock.elapsed() >= 70, qPrintable(QString::number(clock.elapsed())));  // 20 ms + 60 ms, minus timer slack
        Publisher::setRetryDelayScale(0);
    }

    void cancelEndsAPublishThatIsWaitingToRetry()
    {
        // A 503 on every blob with a 100 s pause: the publish sits in its retry pause. Cancel must end it now, not after the pause.
        Publisher::setRetryDelayScale(100);
        Fixture f;
        GatedTarget target(f.host.path());
        target.failWith = [](const Op& op) -> std::optional<int> {
            return op.kind == Op::Kind::Put && op.key.contains("files/") ? std::optional<int>(503) : std::nullopt;
        };
        Publisher publisher(&target, folderConfig(f.host.path()));
        QSignalSpy spy(&publisher, &Publisher::finished);
        publisher.publish(f.job(1, TokenA, std::nullopt), false);
        QTRY_VERIFY_WITH_TIMEOUT(target.refused >= 1, 5000);
        QElapsedTimer clock;
        clock.start();
        publisher.abort();
        QTRY_COMPARE_WITH_TIMEOUT(spy.count(), 1, 5000);
        QVERIFY(clock.elapsed() < 5000);
        QVERIFY(!spy.first().at(0).toBool());
        Publisher::setRetryDelayScale(0);
    }

    void anotherOwnerPublishingMeanwhile()
    {
        Fixture f;
        FolderTarget target(f.host.path());
        Publisher publisher(&target, folderConfig(f.host.path()));
        QVERIFY(publishAndWait(publisher, f.job(1, TokenA, std::nullopt)).first);
        // The other owner got to revision 2 while this owner prepared revision 2 from baseline 1.
        QVERIFY(writeFile(f.packDir(TokenA) + "/latest.json", latestToJson({ FormatVersion, "pack-1", 2 })));
        const QByteArray theirs = readFile(f.packDir(TokenA) + "/latest.json");
        const auto [ok, error] = publishAndWait(publisher, f.job(2, TokenA, 1));
        QVERIFY(!ok);
        QVERIFY(error.contains("Another owner"));
        QCOMPARE(readFile(f.packDir(TokenA) + "/latest.json"), theirs);  // their marker is untouched
    }

    void readLatest()
    {
        Fixture f;
        FolderTarget target(f.host.path());
        Publisher publisher(&target, folderConfig(f.host.path()));
        bool called = false;
        publisher.readLatest(TokenA, [&](bool ok, std::optional<Latest> latest, const QString&) {
            called = true;
            QVERIFY(ok);
            QVERIFY(!latest);  // no pack yet is not an error
        });
        QTRY_VERIFY(called);
        QVERIFY(publishAndWait(publisher, f.job(1, TokenA, std::nullopt)).first);
        called = false;
        publisher.readLatest(TokenA, [&](bool ok, std::optional<Latest> latest, const QString&) {
            called = true;
            QVERIFY(ok && latest);
            QCOMPARE(latest->revision, 1);
        });
        QTRY_VERIFY(called);
    }

    // ---------------------------------------------------------------- rotate
    void rotateMovesThePack()
    {
        Fixture f;
        FolderTarget target(f.host.path());
        Publisher publisher(&target, folderConfig(f.host.path()));
        const PublishJob job = f.job(1, TokenA, std::nullopt);
        QVERIFY(publishAndWait(publisher, job).first);

        QSignalSpy spy(&publisher, &Publisher::finished);
        publisher.rotate(job.manifest, TokenA, TokenB);
        QVERIFY(spy.wait(15000));
        QVERIFY2(spy.first().at(0).toBool(), qPrintable(spy.first().at(1).toString()));
        QVERIFY(spy.first().at(1).toString().isEmpty());

        // The old folder is gone, the new one is complete and says the same pack and revision.
        QVERIFY(!QDir(f.packDir(TokenA)).exists());
        const auto latest = parseLatest(readFile(f.packDir(TokenB) + "/latest.json"));
        QVERIFY(latest);
        QCOMPARE(latest.value->packId, QStringLiteral("pack-1"));
        QCOMPARE(latest.value->revision, 1);
        QCOMPARE(readFile(f.packDir(TokenB) + "/revisions/1.json"), manifestToJson(job.manifest));
        for (const QString& sha : blobsToCheck(job.manifest))
            QCOMPARE(sha512Of(readFile(f.packDir(TokenB) + "/files/" + sha)), sha);
    }

    void failedRotateKeepsTheOldLinkWorking()
    {
        Fixture f;
        const PublishJob job = f.job(1, TokenA, std::nullopt);
        {
            FolderTarget target(f.host.path());
            Publisher publisher(&target, folderConfig(f.host.path()));
            QVERIFY(publishAndWait(publisher, job).first);
        }
        GatedTarget target(f.host.path());
        target.failWith = [](const Op& op) -> std::optional<int> {
            return op.kind == Op::Kind::Copy && op.key.contains("files/") ? std::optional<int>(403) : std::nullopt;
        };
        Publisher publisher(&target, folderConfig(f.host.path()));
        QSignalSpy spy(&publisher, &Publisher::finished);
        publisher.rotate(job.manifest, TokenA, TokenB);
        QVERIFY(spy.wait(15000));
        QVERIFY(!spy.first().at(0).toBool());
        QVERIFY(QFile::exists(f.packDir(TokenA) + "/latest.json"));       // nothing was deleted
        QVERIFY(QFile::exists(f.packDir(TokenA) + "/revisions/1.json"));
        QVERIFY(!QFile::exists(f.packDir(TokenB) + "/latest.json"));      // the new folder never went live
    }

    void rotateWithAFailedDeleteIsAWarning()
    {
        Fixture f;
        const PublishJob job = f.job(1, TokenA, std::nullopt);
        {
            FolderTarget target(f.host.path());
            Publisher publisher(&target, folderConfig(f.host.path()));
            QVERIFY(publishAndWait(publisher, job).first);
        }
        GatedTarget target(f.host.path());
        target.failWith = [](const Op& op) -> std::optional<int> {
            return op.kind == Op::Kind::Delete && op.key.contains("files/") ? std::optional<int>(403) : std::nullopt;
        };
        Publisher publisher(&target, folderConfig(f.host.path()));
        QSignalSpy spy(&publisher, &Publisher::finished);
        publisher.rotate(job.manifest, TokenA, TokenB);
        QVERIFY(spy.wait(15000));
        QVERIFY(spy.first().at(0).toBool());  // the new link works
        QVERIFY(!spy.first().at(1).toString().isEmpty());  // and the user is told what is left behind
        QVERIFY(!QFile::exists(f.packDir(TokenA) + "/latest.json"));  // the old link is dead anyway
        QVERIFY(QFile::exists(f.packDir(TokenB) + "/latest.json"));
    }

    // The Worker answers a file above 100 MB with a vague "Invalid blob batch": the launcher names the files before it sends anything.
    void relayNamesFilesAboveTheServerLimit()
    {
        HostConfig host;
        host.kind = HostConfig::Kind::Relay;
        RelayTarget target(host);
        PublishJob job;
        job.token = TokenA;
        job.manifest.files = {
            File{ "mods/small.jar", 10, sha512Of("s"), {} },
            File{ "mods/huge.jar", 100'000'001, sha512Of("h"), {} },
            // a Modrinth file is fetched from the CDN and never uploaded, so its size does not matter
            File{ "mods/public.jar", 200'000'000, sha512Of("p"), { QStringLiteral("https://cdn.modrinth.com/data/AABBCCDD/versions/11223344/p.jar") } },
        };
        target.setJob(job);
        bool called = false;
        bool ok = true;
        QString error;
        target.prepare(true, [&](bool result, const QString& message) {
            called = true;
            ok = result;
            error = message;
        });
        QTRY_VERIFY(called);
        QVERIFY(!ok);
        QVERIFY2(error.contains(QStringLiteral("mods/huge.jar (100 MB)")), qPrintable(error));
        QVERIFY(!error.contains(QStringLiteral("small.jar")));
        QVERIFY(!error.contains(QStringLiteral("public.jar")));
    }
};

QTEST_GUILESS_MAIN(SharedPackTargetTest)

#include "SharedPackTarget_test.moc"
