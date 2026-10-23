// SPDX-License-Identifier: GPL-3.0-only

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <atomic>
#include <functional>
#include <thread>
#include <vector>

#include "sharedpack/SharedPackManifest.h"
#include "sharedpack/SharedPackPath.h"
#include "sharedpack/SharedPackPlan.h"
#include "sharedpack/SharedPackRedact.h"
#include "sharedpack/SharedPackSigV4.h"
#include "sharedpack/SharedPackState.h"

using namespace SharedPack;

namespace {

QString sha(char c)
{
    return QString(128, QLatin1Char(c));
}

QJsonObject fileJson(const QString& path, char hash = 'a')
{
    return QJsonObject{ { "path", path }, { "size", 10 }, { "sha512", sha(hash) } };
}

QJsonObject validManifest()
{
    return QJsonObject{ { "formatVersion", 1 },
                        { "packId", "pack-1" },
                        { "name", "Friends Pack" },
                        { "revision", 3 },
                        { "changelog", "Added a mod\nRemoved another" },
                        { "someFutureField", 21 },  // readers ignore fields they do not know
                        { "components", QJsonArray{ QJsonObject{ { "uid", "net.minecraft" }, { "version", "1.21.1" } },
                                                    QJsonObject{ { "uid", "net.fabricmc.fabric-loader" }, { "version", "0.16.5" } } } },
                        { "files", QJsonArray{ fileJson("mods/a.jar"), fileJson("config/a.toml", 'b') } } };
}

QByteArray bytes(const QJsonObject& object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

Parsed<Manifest> parse(const QJsonObject& object)
{
    return parseManifest(bytes(object));
}

File file(const QString& path, char hash)
{
    return File{ path, 10, sha(hash), {} };
}

Manifest manifest(int revision, const QList<File>& files)
{
    Manifest m;
    m.packId = "pack-1";
    m.name = "P";
    m.revision = revision;
    m.files = files;
    return m;
}

State state(int revision, const QMap<QString, QString>& installed, Role role = Role::Member)
{
    State s;
    s.packId = "pack-1";
    s.role = role;
    s.revision = revision;
    s.installed = installed;
    return s;
}

DiskState disk(const QMap<QString, QString>& files)
{
    DiskState d;
    for (auto it = files.cbegin(); it != files.cend(); ++it)
        d.insert(it.key().toLower(), DiskEntry{ DiskKind::File, it.value() });
    return d;
}

const Action* find(const Plan& plan, const QString& path)
{
    for (const Action& action : plan.actions) {
        if (action.path == path)
            return &action;
    }
    return nullptr;
}

// Kind and reason of the action for a path, "none" when the plan has no action for it.
QString outcome(const Plan& plan, const QString& path)
{
    const Action* action = find(plan, path);
    if (!action)
        return QStringLiteral("none");
    return QString::number(static_cast<int>(action->kind)) + ':' + QString::number(static_cast<int>(action->why));
}

QString expect(ActionKind kind, Why why)
{
    return QString::number(static_cast<int>(kind)) + ':' + QString::number(static_cast<int>(why));
}

SigV4::Credentials awsCredentials()
{
    return { "AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY" };  // the documentation's example keys, not real
}

// The request every example of the AWS documentation starts from: virtual-hosted bucket, 24 May 2013 00:00:00 GMT, us-east-1.
SigV4::Request awsRequest(const QString& method, const QString& path)
{
    SigV4::Request r;
    r.method = method;
    r.path = path;
    r.headers.insert("host", "examplebucket.s3.amazonaws.com");
    r.time = QDateTime::fromString("2013-05-24T00:00:00Z", Qt::ISODate);
    r.region = "us-east-1";
    return r;
}

}  // namespace

class SharedPackTest : public QObject {
    Q_OBJECT

   private slots:
    // ---- paths ----
    void safePaths_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<bool>("ok");
        const auto add = [](const char* name, const QString& path, bool ok) { QTest::newRow(name) << path << ok; };
        add("mod", "mods/a.jar", true);
        add("config", "config/sodium-options.json", true);
        add("root file", "options.txt", true);
        add("space inside", "config/my file.txt", true);
        add("empty", "", false);
        add("absolute", "/etc/passwd", false);
        add("unc", "//server/share/x", false);
        add("drive letter", "C:/x", false);
        add("drive relative", "C:x", false);
        add("stream", "mods/a.jar:evil", false);
        add("backslash", "mods\\a.jar", false);
        add("dotdot", "../x", false);
        add("dotdot inside", "mods/../../x", false);
        add("dot", "mods/./a.jar", false);
        add("double slash", "mods//a.jar", false);
        add("trailing slash", "mods/", false);
        add("trailing dot", "mods/a.", false);
        add("trailing space", "mods/a ", false);
        add("device", "con", false);
        add("device ext", "config/CON.txt", false);
        add("device nul", "mods/nul.tar.gz", false);
        add("com1", "COM1", false);
        add("lpt9", "lpt9.log", false);
        add("com superscript", QStringLiteral("COM\u00b9"), false);
        add("com10 is fine", "COM10", true);
        add("wildcard", "mods/a*.jar", false);
        add("question", "mods/a?.jar", false);
        add("control char", QStringLiteral("mods/a\x01.jar"), false);
        add("packnew", "config/a.toml.packnew", false);
        add("too long", QString(300, QLatin1Char('a')), false);
        add("segment too long", "mods/" + QString(101, QLatin1Char('a')), false);
    }
    void safePaths()
    {
        QFETCH(QString, path);
        QFETCH(bool, ok);
        QString why;
        QCOMPARE(isSafePath(path, &why), ok);
        QCOMPARE(why.isEmpty(), ok);
    }

    void classifyPaths_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<int>("expected");
        const auto add = [](const char* name, const QString& path, FileClass c) { QTest::newRow(name) << path << static_cast<int>(c); };
        add("mod", "mods/a.jar", FileClass::PackWins);
        add("mod upper case", "MODS/A.JAR", FileClass::PackWins);
        add("resourcepack", "resourcepacks/a.zip", FileClass::PackWins);
        add("shaderpack", "shaderpacks/a.zip", FileClass::PackWins);
        add("config", "config/a.toml", FileClass::Merge);
        add("file named mods", "mods", FileClass::Merge);
        add("root file", "notes.txt", FileClass::Merge);
        add("options", "options.txt", FileClass::SeedOnly);
        add("servers", "Servers.dat", FileClass::SeedOnly);
        add("options in config", "config/options.txt", FileClass::Merge);
        add("saves", "saves/w/level.dat", FileClass::Excluded);
        add("screenshots", "Screenshots/a.png", FileClass::Excluded);
        add("logs", "logs/latest.log", FileClass::Excluded);
        add("crash reports", "crash-reports/c.txt", FileClass::Excluded);
        add("launcher metadata", "mods/.index/a.pw.toml", FileClass::Excluded);
        add("downloaded engine", "rinku-libraries/jcef-v1/windows_amd64/libcef.dll", FileClass::Excluded);
        add("downloaded engine 2", "MCEF-Libraries/x.dll", FileClass::Excluded);
    }
    void classifyPaths()
    {
        QFETCH(QString, path);
        QFETCH(int, expected);
        QCOMPARE(static_cast<int>(classify(path)), expected);
    }

    // ---- manifest ----
    void manifestValid()
    {
        const auto parsed = parse(validManifest());
        QVERIFY2(parsed, qPrintable(parsed.error));
        QCOMPARE(parsed.value->name, QStringLiteral("Friends Pack"));
        QCOMPARE(parsed.value->revision, 3);
        QCOMPARE(parsed.value->components.size(), 2);
        QCOMPARE(parsed.value->files.size(), 2);
        QCOMPARE(parsed.value->totalSize(), qint64(20));
        QVERIFY(parsed.value->changelog.contains('\n'));
    }
    void manifestIgnoresUnknownFields()
    {
        auto m = validManifest();
        m["somethingNew"] = QJsonObject{ { "x", 1 } };
        auto files = m["files"].toArray();
        auto f = files[0].toObject();
        f["extra"] = true;
        files[0] = f;
        m["files"] = files;
        QVERIFY(parse(m));
    }
    void manifestTooNew()
    {
        auto m = validManifest();
        m["formatVersion"] = 2;
        m["files"] = "no longer an array in the future";  // a newer launcher may change anything, we must not look
        const auto parsed = parse(m);
        QVERIFY(!parsed);
        QCOMPARE(parsed.failure, Failure::TooNew);
    }
    void manifestCorrupt_data()
    {
        QTest::addColumn<QByteArray>("json");
        const auto add = [](const char* name, const QByteArray& json) { QTest::newRow(name) << json; };
        const QByteArray good = bytes(validManifest());
        add("empty", "");
        add("not json", "hello");
        add("array", "[]");
        add("truncated", good.left(good.size() / 2));
        add("truncated by one", good.left(good.size() - 1));
        add("trailing junk", good + "x");
        add("too large", QByteArray(Limits::MaxManifestBytes + 1, ' '));
        const auto mutated = [](const char* key, const QJsonValue& value) {
            auto m = validManifest();
            m[key] = value;
            return bytes(m);
        };
        add("no format", mutated("formatVersion", QJsonValue::Undefined));
        add("format zero", mutated("formatVersion", 0));
        add("format string", mutated("formatVersion", "1"));
        add("format fraction", mutated("formatVersion", 1.5));
        add("bad pack id", mutated("packId", "a/b"));
        add("empty pack id", mutated("packId", ""));
        add("empty name", mutated("name", ""));
        add("name with newline", mutated("name", "a\nb"));
        add("name with bidi override", mutated("name", QStringLiteral("a\u202eb")));
        add("name padded", mutated("name", " a"));
        add("name too long", mutated("name", QString(Limits::MaxNameChars + 1, QLatin1Char('a'))));
        add("revision zero", mutated("revision", 0));
        add("revision negative", mutated("revision", -1));
        add("revision fraction", mutated("revision", 1.5));
        add("revision too big", mutated("revision", 4294967296.0));
        add("changelog control char", mutated("changelog", QStringLiteral("a\x01")));
        add("changelog too long", mutated("changelog", QString(Limits::MaxChangelogChars + 1, QLatin1Char('a'))));
        add("files not array", mutated("files", QJsonObject()));
        add("components not array", mutated("components", "x"));
        add("no minecraft", mutated("components", QJsonArray{ QJsonObject{ { "uid", "net.neoforged" }, { "version", "21.1.1" } } }));
        add("unknown component", mutated("components", QJsonArray{ QJsonObject{ { "uid", "net.minecraft" }, { "version", "1.21" } },
                                                                   QJsonObject{ { "uid", "com.evil.loader" }, { "version", "1" } } }));
        add("two loaders", mutated("components", QJsonArray{ QJsonObject{ { "uid", "net.minecraft" }, { "version", "1.21" } },
                                                             QJsonObject{ { "uid", "net.neoforged" }, { "version", "21.1.1" } },
                                                             QJsonObject{ { "uid", "net.minecraftforge" }, { "version", "52.0.0" } } }));
        add("duplicate minecraft", mutated("components", QJsonArray{ QJsonObject{ { "uid", "net.minecraft" }, { "version", "1.21" } },
                                                                     QJsonObject{ { "uid", "net.minecraft" }, { "version", "1.20" } } }));
        add("file not object", mutated("files", QJsonArray{ "mods/a.jar" }));
        add("file traversal", mutated("files", QJsonArray{ fileJson("../evil.dll") }));
        add("file absolute", mutated("files", QJsonArray{ fileJson("/evil.dll") }));
        add("file drive", mutated("files", QJsonArray{ fileJson("C:/evil.dll") }));
        add("file device", mutated("files", QJsonArray{ fileJson("mods/con.jar") }));
        add("file in saves", mutated("files", QJsonArray{ fileJson("saves/w/level.dat") }));
        add("file in index", mutated("files", QJsonArray{ fileJson("mods/.index/a.pw.toml") }));
        add("duplicate path", mutated("files", QJsonArray{ fileJson("mods/a.jar"), fileJson("mods/a.jar", 'b') }));
        add("duplicate path other case", mutated("files", QJsonArray{ fileJson("mods/a.jar"), fileJson("mods/A.JAR", 'b') }));
        add("file and folder", mutated("files", QJsonArray{ fileJson("config"), fileJson("config/a.toml") }));
        add("upper case hash",
            mutated("files", QJsonArray{ QJsonObject{ { "path", "a" }, { "size", 1 }, { "sha512", sha('a').toUpper() } } }));
        add("short hash", mutated("files", QJsonArray{ QJsonObject{ { "path", "a" }, { "size", 1 }, { "sha512", "abc" } } }));
        add("negative size", mutated("files", QJsonArray{ QJsonObject{ { "path", "a" }, { "size", -1 }, { "sha512", sha('a') } } }));
        add("huge size",
            mutated("files",
                    QJsonArray{ QJsonObject{ { "path", "a" }, { "size", double(Limits::MaxFileSize) + 1 }, { "sha512", sha('a') } } }));
        add("size is string", mutated("files", QJsonArray{ QJsonObject{ { "path", "a" }, { "size", "1" }, { "sha512", sha('a') } } }));

        QJsonArray many;
        for (int i = 0; i <= Limits::MaxFiles; ++i)
            many.append(QJsonObject{ { "path", QString("f/%1").arg(i) }, { "size", 1 }, { "sha512", sha('a') } });
        add("too many files", mutated("files", many));

        QJsonArray big;  // each file is within its limit, together they are not
        // 70 x 512 MB = 35 GB, exceeds MaxTotalSize (32 GB)
        for (int i = 0; i < 70; ++i)
            big.append(QJsonObject{ { "path", QString("f/%1").arg(i) }, { "size", double(Limits::MaxFileSize) }, { "sha512", sha('a') } });
        add("total too big", mutated("files", big));
    }
    void manifestCorrupt()
    {
        QFETCH(QByteArray, json);
        const auto parsed = parseManifest(json);
        QVERIFY(!parsed);
        QCOMPARE(parsed.failure, Failure::Corrupt);
        QVERIFY(!parsed.error.isEmpty());
    }

    void manifestUrls_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("ok");
        const auto add = [](const char* name, const QString& url, bool ok) { QTest::newRow(name) << url << ok; };
        add("modrinth", "https://cdn.modrinth.com/data/AANobbMI/versions/abc/sodium.jar", true);
        add("modrinth upper case host", "https://CDN.Modrinth.com/data/x/y.jar", true);
        add("explicit port", "https://cdn.modrinth.com:443/data/x.jar", true);
        add("http", "http://cdn.modrinth.com/data/x.jar", false);
        add("file", "file:///c:/x.jar", false);
        add("other host", "https://example.com/data/x.jar", false);
        add("lookalike host", "https://cdn.modrinth.com.evil.org/data/x.jar", false);
        add("subdomain trick", "https://evil.org/cdn.modrinth.com/data/x.jar", false);
        add("credentials", "https://user:pw@cdn.modrinth.com/data/x.jar", false);
        add("credentials trick", "https://cdn.modrinth.com@evil.org/data/x.jar", false);
        add("other port", "https://cdn.modrinth.com:8443/data/x.jar", false);
        add("fragment", "https://cdn.modrinth.com/data/x.jar#a", false);
        add("not data", "https://cdn.modrinth.com/api/x.jar", false);
        add("ip", "https://127.0.0.1/data/x.jar", false);
        add("too long", "https://cdn.modrinth.com/data/" + QString(2100, QLatin1Char('a')), false);
        add("empty", "", false);
    }
    void manifestUrls()
    {
        QFETCH(QString, url);
        QFETCH(bool, ok);
        QCOMPARE(isAllowedUrl(url), ok);

        auto m = validManifest();
        auto f = fileJson("mods/a.jar");
        f["urls"] = QJsonArray{ url };
        m["files"] = QJsonArray{ f };
        QCOMPARE(bool(parse(m)), ok);
    }
    void manifestTooManyUrls()
    {
        auto m = validManifest();
        auto f = fileJson("mods/a.jar");
        QJsonArray urls;
        for (int i = 0; i <= Limits::MaxUrlsPerFile; ++i)
            urls.append(QString("https://cdn.modrinth.com/data/%1/a.jar").arg(i));
        f["urls"] = urls;
        m["files"] = QJsonArray{ f };
        QVERIFY(!parse(m));
    }

    void latestParsing()
    {
        const QJsonObject good{ { "formatVersion", 1 }, { "packId", "pack-1" }, { "revision", 7 } };
        const auto parsed = parseLatest(bytes(good));
        QVERIFY(parsed);
        QCOMPARE(parsed.value->revision, 7);

        QVERIFY(!parseLatest(bytes(good).left(10)));
        QVERIFY(!parseLatest(""));
        auto bad = good;
        bad["revision"] = 0;
        QVERIFY(!parseLatest(bytes(bad)));
        bad = good;
        bad["packId"] = "../x";
        QVERIFY(!parseLatest(bytes(bad)));
        bad = good;
        bad["formatVersion"] = 9;
        QCOMPARE(parseLatest(bytes(bad)).failure, Failure::TooNew);
        QVERIFY(!parseLatest(QByteArray(Limits::MaxLatestBytes + 1, ' ')));
    }

    // ---- planner: config and other files (merge) ----
    void planConfigNotEditedPackChanged()
    {
        const auto plan = planUpdate(state(1, { { "config/a.toml", sha('a') } }), manifest(2, { file("config/a.toml", 'b') }),
                                     disk({ { "config/a.toml", sha('a') } }));
        QVERIFY(plan);
        QCOMPARE(outcome(plan, "config/a.toml"), expect(ActionKind::Write, Why::PackChanged));
        QCOMPARE(plan.installed.value("config/a.toml"), sha('b'));
    }
    void planConfigUserEditedPackSame()
    {
        const auto plan = planUpdate(state(1, { { "config/a.toml", sha('a') } }), manifest(2, { file("config/a.toml", 'a') }),
                                     disk({ { "config/a.toml", sha('c') } }));
        QCOMPARE(outcome(plan, "config/a.toml"), expect(ActionKind::Keep, Why::UserEdited));
        QCOMPARE(plan.installed.value("config/a.toml"), sha('a'));
    }
    void planConfigBothChanged()
    {
        const auto plan = planUpdate(state(1, { { "config/a.toml", sha('a') } }), manifest(2, { file("config/a.toml", 'b') }),
                                     disk({ { "config/a.toml", sha('c') } }));
        QCOMPARE(outcome(plan, "config/a.toml"), expect(ActionKind::PackNew, Why::Conflict));
        // the pack's version becomes the base, so the next update that leaves the file alone does not warn again
        QCOMPARE(plan.installed.value("config/a.toml"), sha('b'));
        QCOMPARE(find(plan, "config/a.toml")->sha512, sha('b'));
    }
    void planConfigAlreadyCurrent()
    {
        const auto plan = planUpdate(state(1, { { "config/a.toml", sha('a') } }), manifest(2, { file("config/a.toml", 'b') }),
                                     disk({ { "config/a.toml", sha('b') } }));
        QCOMPARE(outcome(plan, "config/a.toml"), expect(ActionKind::Keep, Why::AlreadyCurrent));
        QCOMPARE(plan.installed.value("config/a.toml"), sha('b'));
    }
    void planConfigUserDeleted()
    {
        const auto kept = planUpdate(state(1, { { "config/a.toml", sha('a') } }), manifest(2, { file("config/a.toml", 'a') }), disk({}));
        QCOMPARE(outcome(kept, "config/a.toml"), expect(ActionKind::Keep, Why::UserDeleted));
        const auto restored =
            planUpdate(state(1, { { "config/a.toml", sha('a') } }), manifest(2, { file("config/a.toml", 'b') }), disk({}));
        QCOMPARE(outcome(restored, "config/a.toml"), expect(ActionKind::Write, Why::RestoredChanged));
    }
    void planConfigFirstInstall()
    {
        const auto plan = planUpdate(state(0, {}), manifest(1, { file("config/new.toml", 'a'), file("config/mine.toml", 'b') }),
                                     disk({ { "config/mine.toml", sha('c') } }));
        QCOMPARE(outcome(plan, "config/new.toml"), expect(ActionKind::Write, Why::NewFile));
        // the member had a file of that name before the pack: theirs stays, the pack's goes next to it
        QCOMPARE(outcome(plan, "config/mine.toml"), expect(ActionKind::PackNew, Why::Conflict));
    }

    // ---- planner: pack wins, member extras, seeds, excluded ----
    void planModsPackWins()
    {
        const auto plan =
            planUpdate(state(1, { { "mods/a.jar", sha('a') } }), manifest(2, { file("mods/a.jar", 'b'), file("mods/n.jar", 'c') }),
                       disk({ { "mods/a.jar", sha('z') } }));  // the member changed the jar: the pack still wins
        QCOMPARE(outcome(plan, "mods/a.jar"), expect(ActionKind::Write, Why::PackChanged));
        QCOMPARE(outcome(plan, "mods/n.jar"), expect(ActionKind::Write, Why::NewFile));
    }
    void planMemberExtraModKept()
    {
        const auto plan = planUpdate(state(1, { { "mods/a.jar", sha('a') } }), manifest(2, { file("mods/a.jar", 'a') }),
                                     disk({ { "mods/a.jar", sha('a') }, { "mods/mine.jar", sha('m') } }));
        QCOMPARE(outcome(plan, "mods/mine.jar"), QStringLiteral("none"));
        QVERIFY(!plan.installed.contains("mods/mine.jar"));
        QCOMPARE(plan.actions.size(), 1);
    }
    void planSeedFiles()
    {
        const auto missing = planUpdate(state(1, {}), manifest(2, { file("options.txt", 'a') }), disk({}));
        QCOMPARE(outcome(missing, "options.txt"), expect(ActionKind::Write, Why::SeedMissing));
        const auto present = planUpdate(state(1, {}), manifest(2, { file("options.txt", 'a') }), disk({ { "options.txt", sha('z') } }));
        QCOMPARE(outcome(present, "options.txt"), expect(ActionKind::Keep, Why::SeedExists));
        QVERIFY(!present.installed.contains("options.txt"));  // never tracked, so never deleted later
    }
    void planExcludedNeverTouched()
    {
        const auto plan = planUpdate(state(1, {}), manifest(2, { file("saves/w/level.dat", 'a'), file("logs/latest.log", 'b') }), disk({}));
        QCOMPARE(outcome(plan, "saves/w/level.dat"), expect(ActionKind::Skip, Why::Excluded));
        QCOMPARE(outcome(plan, "logs/latest.log"), expect(ActionKind::Skip, Why::Excluded));
        QVERIFY(plan.installed.isEmpty());
        QVERIFY(blobsNeeded(plan).isEmpty());
    }
    void planUnsafeOnDisk()
    {
        DiskState d;
        d.insert("mods/a.jar", DiskEntry{ DiskKind::Unsafe, {} });  // a symlink, a directory
        const auto plan = planUpdate(state(1, {}), manifest(2, { file("mods/a.jar", 'a') }), d);
        QCOMPARE(outcome(plan, "mods/a.jar"), expect(ActionKind::Skip, Why::UnsafeOnDisk));
    }
    void planCaseInsensitiveDisk()
    {
        const auto plan = planUpdate(state(1, {}), manifest(2, { file("mods/A.jar", 'a') }), disk({ { "mods/a.jar", sha('a') } }));
        QCOMPARE(outcome(plan, "mods/A.jar"), expect(ActionKind::Keep, Why::AlreadyCurrent));
    }

    // ---- planner: files the pack dropped ----
    void planRemovedNotEditedIsDeleted()
    {
        const auto plan = planUpdate(state(1, { { "mods/old.jar", sha('a') }, { "config/old.toml", sha('b') } }), manifest(2, {}),
                                     disk({ { "mods/old.jar", sha('a') }, { "config/old.toml", sha('b') } }));
        QCOMPARE(outcome(plan, "mods/old.jar"), expect(ActionKind::Delete, Why::PackRemoved));
        QCOMPARE(outcome(plan, "config/old.toml"), expect(ActionKind::Delete, Why::PackRemoved));
        QVERIFY(plan.installed.isEmpty());
    }
    void planRemovedButEditedIsKept()
    {
        const auto plan = planUpdate(state(1, { { "config/old.toml", sha('a') }, { "mods/old.jar", sha('b') } }), manifest(2, {}),
                                     disk({ { "config/old.toml", sha('z') }, { "mods/old.jar", sha('y') } }));
        QCOMPARE(outcome(plan, "config/old.toml"), expect(ActionKind::Keep, Why::RemovedButEdited));
        // even a mod: a different hash than the pack installed means it is not the pack's file any more
        QCOMPARE(outcome(plan, "mods/old.jar"), expect(ActionKind::Keep, Why::RemovedButEdited));
        QVERIFY(plan.installed.isEmpty());
    }
    void planRemovedAlreadyGone()
    {
        const auto plan = planUpdate(state(1, { { "mods/old.jar", sha('a') } }), manifest(2, {}), disk({}));
        QCOMPARE(outcome(plan, "mods/old.jar"), QStringLiteral("none"));
        QVERIFY(plan.installed.isEmpty());
    }
    void planDeletesFirstDeepestFirst()
    {
        const auto plan =
            planUpdate(state(1, { { "config/a/b/c.toml", sha('a') }, { "config/a/b.toml", sha('b') }, { "mods/x.jar", sha('c') } }),
                       manifest(2, { file("mods/new.jar", 'd') }),
                       disk({ { "config/a/b/c.toml", sha('a') }, { "config/a/b.toml", sha('b') }, { "mods/x.jar", sha('c') } }));
        QCOMPARE(plan.actions.size(), 4);
        for (int i = 0; i < 3; ++i)
            QCOMPARE(plan.actions[i].kind, ActionKind::Delete);
        QCOMPARE(plan.actions[3].kind, ActionKind::Write);
        QVERIFY(plan.actions[0].path.toLower() > plan.actions[1].path.toLower());
        QVERIFY(plan.actions[1].path.toLower() > plan.actions[2].path.toLower());
    }

    // ---- planner: refusals and hostile input ----
    void planOwnerNeverUpdates()
    {
        const auto plan = planUpdate(state(1, { { "mods/a.jar", sha('a') } }, Role::Owner), manifest(2, { file("mods/a.jar", 'b') }),
                                     disk({ { "mods/a.jar", sha('a') } }));
        QVERIFY(!plan);
        QCOMPARE(plan.refusal, Plan::Refusal::OwnerRole);
        QVERIFY(plan.actions.isEmpty());
        QVERIFY(plan.installed.isEmpty());
    }
    void planRevisionGoingBackwards()
    {
        const auto plan = planUpdate(state(5, {}), manifest(4, { file("mods/a.jar", 'a') }), disk({}));
        QVERIFY(!plan);
        QCOMPARE(plan.refusal, Plan::Refusal::Backwards);
        QVERIFY(plan.actions.isEmpty());
        // the same revision is a re-apply (repairing an interrupted update), not a refusal
        QVERIFY(planUpdate(state(5, {}), manifest(5, { file("mods/a.jar", 'a') }), disk({})));
    }
    void planWrongPack()
    {
        auto next = manifest(2, { file("mods/a.jar", 'a') });
        next.packId = "other-pack";
        const auto plan = planUpdate(state(1, {}), next, disk({}));
        QCOMPARE(plan.refusal, Plan::Refusal::WrongPack);
        QVERIFY(plan.actions.isEmpty());
    }
    void planTraversalSkipped()
    {
        // parseManifest never lets these through; the planner is the last line of defence and must not act on them either
        const QStringList bad = { "../evil.jar",        "mods/../../evil.jar", "/etc/passwd", "C:/evil.jar",
                                  "mods\\..\\evil.jar", "mods/con.jar",        "mods/a.jar:s" };
        QList<File> files;
        for (const QString& path : bad)
            files.append(file(path, 'a'));
        const auto plan = planUpdate(state(1, {}), manifest(2, files), disk({}));
        QCOMPARE(plan.actions.size(), static_cast<int>(bad.size()));
        for (const QString& path : bad)
            QCOMPARE(outcome(plan, path), expect(ActionKind::Skip, Why::BadPath));
        QVERIFY(plan.installed.isEmpty());
        QVERIFY(blobsNeeded(plan).isEmpty());
    }
    void planTamperedStateCannotDelete()
    {
        // a hand-edited state file must not turn a "pack removed this" into a delete outside the game folder
        const auto plan = planUpdate(state(1, { { "../victim.txt", sha('a') } }), manifest(2, {}), disk({ { "../victim.txt", sha('a') } }));
        QCOMPARE(outcome(plan, "../victim.txt"), expect(ActionKind::Skip, Why::BadPath));
        for (const Action& action : plan.actions)
            QVERIFY(action.kind != ActionKind::Delete);
    }

    // ---- planner: whole updates ----
    void planIsIdempotent()
    {
        const auto next = manifest(2, { file("mods/a.jar", 'a'), file("config/a.toml", 'b'), file("options.txt", 'c') });
        const auto first = planUpdate(state(1, {}), next, disk({}));
        QVERIFY(first);
        // apply it: the disk now holds what the plan wrote, and the state is the plan's `installed`
        QMap<QString, QString> onDisk;
        for (const Action& action : first.actions) {
            if (action.kind == ActionKind::Write)
                onDisk.insert(action.path, action.sha512);
        }
        const auto second = planUpdate(state(2, first.installed), next, disk(onDisk));
        QVERIFY(second);
        for (const Action& action : second.actions)
            QCOMPARE(action.kind, ActionKind::Keep);
        QCOMPARE(second.installed, first.installed);
    }
    void planInterruptedApplyIsRepaired()
    {
        // the state file is written last, so a crash leaves the OLD state with a half-applied disk: re-planning must finish the job
        const auto old = state(1, { { "mods/a.jar", sha('a') }, { "config/a.toml", sha('b') } });
        const auto next = manifest(2, { file("mods/a.jar", 'x'), file("config/a.toml", 'y'), file("mods/new.jar", 'n') });
        const auto plan =
            planUpdate(old, next, disk({ { "mods/a.jar", sha('x') }, { "config/a.toml", sha('b') } }));  // only the jar made it
        QCOMPARE(outcome(plan, "mods/a.jar"), expect(ActionKind::Keep, Why::AlreadyCurrent));
        QCOMPARE(outcome(plan, "config/a.toml"), expect(ActionKind::Write, Why::PackChanged));
        QCOMPARE(outcome(plan, "mods/new.jar"), expect(ActionKind::Write, Why::NewFile));
    }
    void blobsNeededListsEachOnce()
    {
        const auto plan =
            planUpdate(state(1, {}),
                       manifest(2, { file("mods/a.jar", 'b'), file("mods/b.jar", 'b'), file("mods/c.jar", 'a'), file("mods/d.jar", 'c') }),
                       disk({ { "mods/d.jar", sha('c') } }));  // d is current: no download
        QCOMPARE(blobsNeeded(plan), QStringList({ sha('a'), sha('b') }));
    }
    void blobsNeededIncludesPackNew()
    {
        const auto plan = planUpdate(state(1, { { "config/a.toml", sha('a') } }), manifest(2, { file("config/a.toml", 'b') }),
                                     disk({ { "config/a.toml", sha('c') } }));
        QCOMPARE(blobsNeeded(plan), QStringList({ sha('b') }));
    }

    // ---- state file ----
    void stateRoundTrip()
    {
        const auto original = state(7, { { "mods/a.jar", sha('a') }, { "config/b.toml", sha('b') } });
        const auto parsed = parseState(stateToJson(original));
        QVERIFY2(parsed, qPrintable(parsed.error));
        QCOMPARE(parsed.value->packId, original.packId);
        QCOMPARE(parsed.value->revision, 7);
        QCOMPARE(parsed.value->installed, original.installed);
        QVERIFY(parsed.value->role == Role::Member);
        const auto owner = parseState(stateToJson(state(1, {}, Role::Owner)));
        QVERIFY(owner);
        QVERIFY(owner.value->role == Role::Owner);
    }
    void stateCorrupt()
    {
        const QByteArray good = stateToJson(state(1, { { "mods/a.jar", sha('a') } }));
        QVERIFY(!parseState(good.left(good.size() / 2)));
        QVERIFY(!parseState(""));
        QVERIFY(!parseState("[]"));
        const auto mutate = [&good](const std::function<void(QJsonObject&)>& change) {
            QJsonObject object = QJsonDocument::fromJson(good).object();
            change(object);
            return parseState(QJsonDocument(object).toJson());
        };
        QVERIFY(!mutate([](QJsonObject& o) { o["role"] = "admin"; }));
        QVERIFY(!mutate([](QJsonObject& o) { o["packId"] = "../x"; }));
        QVERIFY(!mutate([](QJsonObject& o) { o["revision"] = -1; }));
        QVERIFY(!mutate([](QJsonObject& o) { o["revision"] = 1.5; }));
        QVERIFY(!mutate([](QJsonObject& o) { o["installed"] = QJsonObject{ { "../x", sha('a') } }; }));
        QVERIFY(
            !mutate([](QJsonObject& o) { o["installed"] = QJsonObject{ { "saves/w/level.dat", sha('a') } }; }));  // excluded: never deleted
        QVERIFY(!mutate([](QJsonObject& o) { o["installed"] = QJsonObject{ { "mods/a.jar", "short" } }; }));
        QVERIFY(!mutate([](QJsonObject& o) { o["installed"] = QJsonObject{ { "mods/a.jar", sha('a') }, { "mods/A.jar", sha('b') } }; }));
    }
    void stateTooNew()
    {
        QJsonObject object = QJsonDocument::fromJson(stateToJson(state(1, {}))).object();
        object["formatVersion"] = 99;
        const auto parsed = parseState(QJsonDocument(object).toJson());
        QVERIFY(!parsed);
        QCOMPARE(parsed.failure, Failure::TooNew);
    }

    // ---- revision selection ----
    void updateCheck()
    {
        const auto latest = [](const QString& packId, int revision) {
            Latest l;
            l.packId = packId;
            l.revision = revision;
            return l;
        };
        QCOMPARE(checkForUpdate(state(3, {}), latest("pack-1", 4)), UpdateCheck::Available);
        QCOMPARE(checkForUpdate(state(3, {}), latest("pack-1", 9)), UpdateCheck::Available);
        QCOMPARE(checkForUpdate(state(3, {}), latest("pack-1", 3)), UpdateCheck::UpToDate);
        QCOMPARE(checkForUpdate(state(3, {}), latest("pack-1", 2)), UpdateCheck::HostBehind);
        QCOMPARE(checkForUpdate(state(3, {}), latest("pack-2", 99)), UpdateCheck::WrongPack);
        QCOMPARE(checkForUpdate(state(0, {}), latest("pack-1", 1)), UpdateCheck::Available);
        QCOMPARE(checkForUpdate(state(3, {}, Role::Owner), latest("pack-1", 4)), UpdateCheck::OwnerNeverUpdates);
    }

    // ---- SigV4: the examples in "Signature Calculations for the Authorization Header" (AWS S3 API reference) ----
    void sigv4Get()
    {
        // GET Object with a Range header
        SigV4::Request r = awsRequest("GET", "/test.txt");
        r.headers.insert("range", "bytes=0-9");
        r.payloadHash = SigV4::emptyPayloadHash();
        const auto signature = SigV4::sign(awsCredentials(), r);
        QCOMPARE(signature.amzDate, QStringLiteral("20130524T000000Z"));
        QVERIFY2(signature.authorization.endsWith("Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41"),
                 qPrintable(signature.authorization));
        QVERIFY(
            signature.authorization.startsWith("AWS4-HMAC-SHA256 "
                                               "Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/"
                                               "aws4_request,SignedHeaders=host;range;x-amz-content-sha256;x-amz-date,"));
    }
    void sigv4Put()
    {
        // PUT Object whose key needs encoding ('$' -> %24) with an extra signed header
        const QByteArray body = "Welcome to Amazon S3.";
        QCOMPARE(SigV4::sha256Hex(body), QStringLiteral("44ce7dd67c959e0d3524ffac1771dfbba87d2b6b4b4e99e42034a8b803f8b072"));
        SigV4::Request r = awsRequest("PUT", "/test$file.text");
        r.headers.insert("date", "Fri, 24 May 2013 00:00:00 GMT");
        r.headers.insert("x-amz-storage-class", "REDUCED_REDUNDANCY");
        r.payloadHash = SigV4::sha256Hex(body);
        QVERIFY(SigV4::canonicalRequest(r, "20130524T000000Z").startsWith("PUT\n/test%24file.text\n\n"));
        const auto signature = SigV4::sign(awsCredentials(), r);
        QVERIFY2(signature.authorization.endsWith("Signature=98ad721746da40c64f1a55b78f14c238d841ea1380cd77a1b5971af0ece108bd"),
                 qPrintable(signature.authorization));
        QVERIFY(signature.authorization.contains("SignedHeaders=date;host;x-amz-content-sha256;x-amz-date;x-amz-storage-class,"));
    }
    void sigv4SubResource()
    {
        // GET Bucket lifecycle: "?lifecycle" has an empty value and still counts as a query parameter
        SigV4::Request r = awsRequest("GET", "/");
        r.query = { { "lifecycle", "" } };
        r.payloadHash = SigV4::emptyPayloadHash();
        const auto signature = SigV4::sign(awsCredentials(), r);
        QVERIFY2(signature.authorization.endsWith("Signature=fea454ca298b7da1c68078a5d1bdbfbbe0d65c699e0f91ac7a200a0136783543"),
                 qPrintable(signature.authorization));
    }
    void sigv4QuerySorted()
    {
        // GET Bucket (list objects) with two parameters given in the "wrong" order
        SigV4::Request r = awsRequest("GET", "/");
        r.query = { { "prefix", "J" }, { "max-keys", "2" } };
        r.payloadHash = SigV4::emptyPayloadHash();
        const auto signature = SigV4::sign(awsCredentials(), r);
        QVERIFY2(signature.authorization.endsWith("Signature=34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7"),
                 qPrintable(signature.authorization));
    }
    void sigv4UriEncode()
    {
        QCOMPARE(SigV4::uriEncode("a b/c", false), QStringLiteral("a%20b/c"));
        QCOMPARE(SigV4::uriEncode("a b/c", true), QStringLiteral("a%20b%2Fc"));
        QCOMPARE(SigV4::uriEncode("AZaz09-._~", true), QStringLiteral("AZaz09-._~"));
        QCOMPARE(SigV4::uriEncode("$&+,:;=?@", true), QStringLiteral("%24%26%2B%2C%3A%3B%3D%3F%40"));  // upper-case hex
        QCOMPARE(SigV4::uriEncode(QStringLiteral("\u00e9"), true), QStringLiteral("%C3%A9"));          // UTF-8 bytes
    }
    void sigv4NeedsHostAndIsDeterministic()
    {
        SigV4::Request r = awsRequest("GET", "/x");
        r.payloadHash = SigV4::emptyPayloadHash();
        const auto a = SigV4::sign(awsCredentials(), r);
        QCOMPARE(SigV4::sign(awsCredentials(), r).authorization, a.authorization);
        auto other = r;
        other.region = "auto";  // Cloudflare R2: another region is another scope and another signature
        const auto b = SigV4::sign(awsCredentials(), other);
        QVERIFY(b.authorization != a.authorization);
        QVERIFY(b.authorization.contains("/20130524/auto/s3/aws4_request,"));
        auto noHost = r;
        noHost.headers.remove("host");
        QVERIFY(SigV4::sign(awsCredentials(), noHost).authorization.isEmpty());
    }
    void sigv4CallerCannotOverrideSignedDefaults()
    {
        // a caller that passes its own x-amz-date or payload hash must not be able to send something other than what is signed
        SigV4::Request r = awsRequest("GET", "/x");
        r.payloadHash = SigV4::emptyPayloadHash();
        const auto plain = SigV4::sign(awsCredentials(), r);
        r.headers.insert("x-amz-date", "20990101T000000Z");
        r.headers.insert("x-amz-content-sha256", "UNSIGNED-PAYLOAD");
        QCOMPARE(SigV4::sign(awsCredentials(), r).authorization, plain.authorization);
    }

    // ---- redaction ----
    void redactTokenAndEncodedSpelling()
    {
        Redactor redactor;
        redactor.add("tok/en+abc=defg");
        const QString text = "GET https://host/p/tok/en+abc=defg/latest.json and https://host/p/tok%2Fen%2Babc%3Ddefg/x";
        const QString result = redactor.apply(text);
        QVERIFY2(!result.contains("tok/en"), qPrintable(result));
        QVERIFY2(!result.contains("tok%2Fen"), qPrintable(result));
        QVERIFY(result.contains("latest.json"));
    }
    void redactIgnoresShortSecrets()
    {
        Redactor redactor;
        redactor.add("abc");
        redactor.add("");
        QCOMPARE(redactor.apply("abc abc"), QStringLiteral("abc abc"));
    }
    void redactLongestFirst()
    {
        Redactor redactor;
        redactor.add("secret-key-1");
        redactor.add("secret-key-1-and-more");
        const QString result = redactor.apply("x secret-key-1-and-more y");
        QVERIFY2(!result.contains("more"), qPrintable(result));  // the longer secret goes whole, no tail left behind
    }
    void redactForLogIsGlobalAndThreadSafe()
    {
        QCOMPARE(redactForLog("nothing registered yet"), QStringLiteral("nothing registered yet"));
        registerLogSecret("global-secret-token");
        std::vector<std::thread> threads;
        std::atomic<int> leaks{ 0 };
        for (int i = 0; i < 4; ++i) {
            threads.emplace_back([&leaks, i] {
                for (int n = 0; n < 200; ++n) {
                    registerLogSecret(QStringLiteral("other-secret-%1-%2").arg(i).arg(n));
                    if (redactForLog("GET /global-secret-token/latest.json").contains("global-secret-token"))
                        ++leaks;
                }
            });
        }
        for (auto& thread : threads)
            thread.join();
        QCOMPARE(leaks.load(), 0);
        QVERIFY(!redactForLog("other-secret-3-199").contains("other-secret"));
    }
};

QTEST_GUILESS_MAIN(SharedPackTest)

#include "SharedPack_test.moc"
