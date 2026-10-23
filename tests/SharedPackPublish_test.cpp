// SPDX-License-Identifier: GPL-3.0-only

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#ifdef Q_OS_WIN
#include <windows.h>
#include <aclapi.h>
#include <string>
#endif
#include <vector>

#include "sharedpack/SharedPackBuild.h"
#include "sharedpack/SharedPackCredentials.h"
#include "sharedpack/SharedPackOwner.h"
#include "sharedpack/SharedPackPublishPlan.h"
#include "sharedpack/SharedPackRedact.h"
#include "sharedpack/SharedPackScan.h"
#include <QCryptographicHash>

using namespace SharedPack;

namespace {

QString hashOf(char c)
{
    return QString(128, QLatin1Char(c));
}

File file(const QString& path, qint64 size, char hash, QStringList urls = {})
{
    return File{ path, size, hashOf(hash), std::move(urls) };
}

QString modrinthUrl(const QString& name)
{
    return QStringLiteral("https://cdn.modrinth.com/data/AABBCCDD/versions/11223344/") + name;
}

BuildInput baseInput()
{
    BuildInput input;
    input.packId = "pack-1";
    input.name = "Friends Pack";
    input.revision = 3;
    input.changelog = "Added a mod";
    input.components = { { "net.minecraft", "1.21.1" }, { "net.fabricmc.fabric-loader", "0.16.5" } };
    return input;
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

// Bytes that count up, so a test knows what the "random" source gave.
RandomBytes counting(int start = 1)
{
    auto counter = std::make_shared<int>(start);
    return [counter](int count) {
        QByteArray bytes;
        for (int i = 0; i < count; ++i)
            bytes.append(static_cast<char>((*counter)++ & 0xff));
        return bytes;
    };
}

Manifest sampleManifest()
{
    BuildInput input = baseInput();
    input.files = { file("mods/big.jar", 5000, '1'), file("mods/small.jar", 100, '2'), file("mods/public.jar", 700, '3', { modrinthUrl("p.jar") }),
                    file("config/same-as-small.toml", 100, '2'), file("config/mid.toml", 900, '4') };
    return *buildManifest(input).manifest;
}

}  // namespace

class SharedPackPublishTest : public QObject {
    Q_OBJECT

   private slots:
    // ---------------------------------------------------------------- manifest builder
    void buildAppliesFilePolicy()
    {
        BuildInput input = baseInput();
        input.files = { file("mods/b.jar", 10, '1'),
                        file("mods/a.jar", 10, '2', { modrinthUrl("a.jar"), "https://evil.example.com/a.jar" }),
                        file("config/x.toml", 5, '3'),
                        file("options.txt", 5, '4'),
                        file("saves/world/level.dat", 5, '5'),
                        file("screenshots/s.png", 5, '6'),
                        file("logs/latest.log", 5, '7'),
                        file("mods/.index/a.pw.toml", 5, '8'),
                        file(".fabric/processedMods/x.jar", 5, '9'),
                        file("config/old.packnew", 5, 'a'),
                        file("config/con.txt", 5, 'b') };
        const BuildResult result = buildManifest(input);
        QVERIFY2(result, qPrintable(result.error));
        QStringList paths;
        for (const File& f : result.manifest->files)
            paths.append(f.path);
        QCOMPARE(paths, (QStringList{ "config/x.toml", "mods/a.jar", "mods/b.jar", "options.txt" }));
        QCOMPARE(result.skipped.size(), 7);
        // Only the Modrinth URL survives; a file with one needs no blob of its own.
        QCOMPARE(result.manifest->files.at(1).urls, QStringList{ modrinthUrl("a.jar") });
        QVERIFY(!needsBlob(result.manifest->files.at(1)));
        QVERIFY(needsBlob(result.manifest->files.at(0)));
    }

    void buildOutputIsWhatAMemberParses()
    {
        BuildInput input = baseInput();
        input.changelog = "Line one\r\nLine two\rLine three";
        input.files = { file("mods/a.jar", 10, '1', { modrinthUrl("a.jar") }), file("config/x.toml", 5, '2') };
        const BuildResult result = buildManifest(input);
        QVERIFY2(result, qPrintable(result.error));
        QCOMPARE(result.manifest->changelog, QStringLiteral("Line one\nLine two\nLine three"));
        const auto parsed = parseManifest(manifestToJson(*result.manifest));
        QVERIFY(parsed);
        QCOMPARE(manifestToJson(*parsed.value), manifestToJson(*result.manifest));
        QCOMPARE(parsed.value->revision, 3);
        QCOMPARE(parsed.value->components.size(), 2);
    }

    void buildIsDeterministic()
    {
        BuildInput input = baseInput();
        input.files = { file("mods/b.jar", 10, '1'), file("mods/a.jar", 10, '2') };
        const QByteArray first = manifestToJson(*buildManifest(input).manifest);
        std::reverse(input.files.begin(), input.files.end());
        QCOMPARE(manifestToJson(*buildManifest(input).manifest), first);
    }

    void buildRejectsWhatAMemberWouldReject()
    {
        BuildInput input = baseInput();
        input.files = { file("mods/a.jar", 10, '1') };

        BuildInput bad = input;
        bad.name = "   ";
        QVERIFY(!buildManifest(bad));
        bad = input;
        bad.name = QString(Limits::MaxNameChars + 1, QLatin1Char('x'));
        QVERIFY(!buildManifest(bad));
        bad = input;
        bad.changelog = QString(Limits::MaxChangelogChars + 1, QLatin1Char('x'));
        QVERIFY(!buildManifest(bad));
        bad = input;
        bad.changelog = "tab\tis fine, bell \a is not";
        QVERIFY(!buildManifest(bad));
        bad = input;
        bad.components = { { "net.fabricmc.fabric-loader", "0.16.5" } };  // no Minecraft
        QVERIFY(!buildManifest(bad));
        bad = input;
        bad.components = { { "net.minecraft", "1.21.1" }, { "net.fabricmc.fabric-loader", "1" }, { "net.neoforged", "2" } };
        QVERIFY(!buildManifest(bad));
        bad = input;
        bad.packId = "not a valid id";
        QVERIFY(!buildManifest(bad));
        bad = input;
        bad.revision = 0;
        QVERIFY(!buildManifest(bad));

        bad = input;
        bad.files = { file("mods/a.jar", Limits::MaxFileSize + 1, '1') };
        QVERIFY(!buildManifest(bad));
        bad.files = { File{ "mods/a.jar", 1, "xyz", {} } };
        QVERIFY(!buildManifest(bad));
        bad.files = { file("mods/A.jar", 1, '1'), file("mods/a.jar", 1, '2') };
        QVERIFY(!buildManifest(bad));
        bad.files = { file("config", 1, '1'), file("config/a.toml", 1, '2') };  // a file and a folder of the same name
        QVERIFY(!buildManifest(bad));
    }

    void buildEnforcesLimits()
    {
        BuildInput input = baseInput();
        for (int i = 0; i <= Limits::MaxFiles; ++i)
            input.files.append(file(QStringLiteral("config/f%1.txt").arg(i), 1, '1'));
        QVERIFY(!buildManifest(input));
        input.files.removeLast();
        QVERIFY(buildManifest(input));

        input.files.clear();
        // 70 x 512 MB = 35 GB, exceeds MaxTotalSize (32 GB)
        for (int i = 0; i < 70; ++i)
            input.files.append(file(QStringLiteral("mods/m%1.jar").arg(i), Limits::MaxFileSize, static_cast<char>('0' + i)));
        QVERIFY(!buildManifest(input));
    }

    // ---------------------------------------------------------------- revision numbers
    void revisionNumbersNeverGoBackwards()
    {
        QCOMPARE(nextRevision(std::nullopt, 0), 1);            // first publish
        QCOMPARE(nextRevision(5, 5), 6);                       // the normal case
        QCOMPARE(nextRevision(5, 7), 8);                       // the host is behind what members may already have: never reuse 6 or 7
        QCOMPARE(nextRevision(9, 2), 10);                      // another owner published meanwhile
        QCOMPARE(nextRevision(std::nullopt, 4), 5);            // the host lost latest.json
        QCOMPARE(nextRevision(Limits::MaxRevision, 0), 0);     // no number left
    }

    void rollbackIsANewRevisionWithOldContent()
    {
        const Manifest old = sampleManifest();
        const Manifest back = rollbackManifest(old, nextRevision(7, 7), "Rollback to revision 3");
        QCOMPARE(back.revision, 8);
        QCOMPARE(back.files.size(), old.files.size());
        QCOMPARE(back.changelog, QStringLiteral("Rollback to revision 3"));
        QCOMPARE(back.packId, old.packId);
        QVERIFY(parseManifest(manifestToJson(back)));
    }

    void twoOwnersAtOnce()
    {
        // Documented limit: last write wins. The publisher only narrows the window by reading latest.json again before it writes it.
        QVERIFY(latestUnchanged(5, 5));
        QVERIFY(latestUnchanged(std::nullopt, std::nullopt));
        QVERIFY(!latestUnchanged(5, 6));
        QVERIFY(!latestUnchanged(std::nullopt, 1));
        QVERIFY(!latestUnchanged(5, std::nullopt));
    }

    // ---------------------------------------------------------------- upload plan
    void uploadPlanOrder()
    {
        const Manifest manifest = sampleManifest();
        QCOMPARE(blobsToCheck(manifest), (QStringList{ hashOf('1'), hashOf('2'), hashOf('4') }));  // the public jar and the duplicate are not extra

        QHash<QString, LocalBlob> local{ { hashOf('1'), { "C:/g/mods/big.jar", 5000 } },
                                         { hashOf('2'), { "C:/g/mods/small.jar", 100 } },
                                         { hashOf('4'), { "C:/g/config/mid.toml", 900 } } };
        const UploadPlan plan = planUpload(manifest, {}, local);
        QVERIFY2(plan, qPrintable(plan.error));
        QCOMPARE(plan.blobCount, 3);
        QCOMPARE(plan.blobBytes, qint64(6000));
        QCOMPARE(plan.puts.size(), 5);
        QCOMPARE(plan.puts.at(0).key, QStringLiteral("files/") + hashOf('1'));  // biggest first
        QCOMPARE(plan.puts.at(1).key, QStringLiteral("files/") + hashOf('4'));
        QCOMPARE(plan.puts.at(2).key, QStringLiteral("files/") + hashOf('2'));
        QCOMPARE(plan.puts.at(3).key, QStringLiteral("revisions/3.json"));
        QCOMPARE(plan.puts.at(3).kind, Put::Kind::Revision);
        QCOMPARE(plan.puts.at(4).key, QStringLiteral("latest.json"));  // last, always
        QCOMPARE(plan.puts.at(4).kind, Put::Kind::Latest);
        for (int i = 0; i < 3; ++i)
            QCOMPARE(plan.puts.at(i).kind, Put::Kind::Blob);

        // What is put is what a member reads.
        QCOMPARE(parseManifest(plan.puts.at(3).body).value->revision, 3);
        const auto latest = parseLatest(plan.puts.at(4).body);
        QVERIFY(latest);
        QCOMPARE(latest.value->revision, 3);
        QCOMPARE(latest.value->packId, QStringLiteral("pack-1"));
    }

    void uploadPlanSkipsWhatTheHostHas()
    {
        const Manifest manifest = sampleManifest();
        QHash<QString, LocalBlob> local{ { hashOf('1'), { "a", 5000 } }, { hashOf('2'), { "b", 100 } }, { hashOf('4'), { "c", 900 } } };
        const UploadPlan plan = planUpload(manifest, { hashOf('1'), hashOf('2') }, local);
        QVERIFY(plan);
        QCOMPARE(plan.blobCount, 1);  // resumable: a repeat run only uploads what is missing
        QCOMPARE(plan.puts.size(), 3);

        const UploadPlan nothing = planUpload(manifest, { hashOf('1'), hashOf('2'), hashOf('4') }, {});
        QVERIFY(nothing);  // everything is there already: no local file needed
        QCOMPARE(nothing.puts.size(), 2);
    }

    void uploadPlanNeedsEveryBlobSomewhere()
    {
        const UploadPlan plan = planUpload(sampleManifest(), { hashOf('1') }, { { hashOf('2'), { "b", 100 } } });
        QVERIFY(!plan);
        QVERIFY(plan.error.contains("config/mid.toml"));
        QVERIFY(plan.puts.isEmpty());
    }

    void sequencerKeepsTheOrder()
    {
        QHash<QString, LocalBlob> local{ { hashOf('1'), { "a", 5000 } }, { hashOf('2'), { "b", 100 } }, { hashOf('4'), { "c", 900 } } };
        UploadSequencer sequencer(planUpload(sampleManifest(), {}, local), 2);

        QCOMPARE(sequencer.next(), (QList<int>{ 0, 1 }));  // bounded: two at a time
        QVERIFY(sequencer.next().isEmpty());
        sequencer.done(1, true);
        QCOMPARE(sequencer.next(), (QList<int>{ 2 }));
        sequencer.done(2, true);
        QVERIFY(sequencer.next().isEmpty());  // the revision waits for blob 0, which is still running
        sequencer.done(0, true);
        QCOMPARE(sequencer.next(), (QList<int>{ 3 }));  // the revision, alone
        QVERIFY(sequencer.next().isEmpty());            // latest.json waits for the revision
        QVERIFY(!sequencer.finished());
        sequencer.done(3, true);
        QCOMPARE(sequencer.next(), (QList<int>{ 4 }));
        sequencer.done(4, true);
        QVERIFY(sequencer.finished());
        QVERIFY(sequencer.succeeded());
        QVERIFY(sequencer.next().isEmpty());
    }

    void failedBlobNeverReachesLatest()
    {
        QHash<QString, LocalBlob> local{ { hashOf('1'), { "a", 5000 } }, { hashOf('2'), { "b", 100 } }, { hashOf('4'), { "c", 900 } } };
        UploadSequencer sequencer(planUpload(sampleManifest(), {}, local), 3);
        QCOMPARE(sequencer.next(), (QList<int>{ 0, 1, 2 }));
        sequencer.done(1, false);
        QVERIFY(sequencer.failed());
        QVERIFY(!sequencer.finished());  // two are still running
        sequencer.done(0, true);
        sequencer.done(2, true);
        QVERIFY(sequencer.finished());
        QVERIFY(!sequencer.succeeded());
        QVERIFY(sequencer.next().isEmpty());  // neither revisions/3.json nor latest.json was ever started
    }

    void failedRevisionNeverReachesLatest()
    {
        UploadSequencer sequencer(planUpload(sampleManifest(), { hashOf('1'), hashOf('2'), hashOf('4') }, {}), 4);
        QCOMPARE(sequencer.next(), (QList<int>{ 0 }));  // the revision, alone
        sequencer.done(0, false);
        QVERIFY(sequencer.finished());
        QVERIFY(sequencer.next().isEmpty());
    }

    // ---------------------------------------------------------------- rotate
    void rotatePlan()
    {
        const Manifest manifest = sampleManifest();  // revision 3
        const RotatePlan plan = planRotate(manifest);
        QCOMPARE(plan.copyKeys.size(), 5);  // 3 blobs, the revision, latest.json
        QCOMPARE(plan.copyKeys.last(), QStringLiteral("latest.json"));
        QCOMPARE(plan.copyKeys.at(3), QStringLiteral("revisions/3.json"));
        QCOMPARE(plan.deleteKeys.first(), QStringLiteral("latest.json"));  // the old link stops working first
        QVERIFY(plan.deleteKeys.contains("revisions/1.json"));
        QVERIFY(plan.deleteKeys.contains("revisions/2.json"));
        QVERIFY(plan.deleteKeys.contains("revisions/3.json"));
        QVERIFY(plan.deleteKeys.contains(QStringLiteral("files/") + hashOf('4')));
        QVERIFY(!plan.deleteKeys.contains(QStringLiteral("files/") + hashOf('3')));  // that file has a public URL: it never had a blob
        // Everything that is copied is also deleted afterwards, and nothing is deleted before it was copied (delete comes after copy).
        for (const QString& key : plan.copyKeys)
            QVERIFY(plan.deleteKeys.contains(key));
    }

    // ---------------------------------------------------------------- invites
    void newPackInviteIsComplete()
    {
        QString error;
        const auto invite = newPackInvite(s3Config(), "  Friends Pack ", counting(), &error);
        QVERIFY2(invite, qPrintable(error));
        QVERIFY(isValidPackId(invite->packId));
        QVERIFY(isValidToken(invite->token));
        QCOMPARE(invite->token.size(), 22);  // 16 bytes, 128 bits
        QVERIFY(invite->packId != invite->token);
        QCOMPARE(invite->name, QStringLiteral("Friends Pack"));
        QCOMPARE(invite->mode(), Invite::Mode::S3);
        // The members get the READ key, never the write key.
        QCOMPARE(invite->accessKeyId, QStringLiteral("READKEYID01234567890"));
        const QString text = inviteToString(*invite);
        QVERIFY(!text.contains("WRITEKEYID"));
        const auto back = parseInvite(text);
        QVERIFY(back);
        QCOMPARE(back.value->token, invite->token);
        QCOMPARE(back.value->bucket, QStringLiteral("mypacks"));
        QCOMPARE(back.value->prefix, QStringLiteral("shared"));
    }

    void folderInviteIsPublic()
    {
        const auto invite = newPackInvite(folderConfig("C:/www/packs"), "Folder Pack", counting());
        QVERIFY(invite);
        QCOMPARE(invite->mode(), Invite::Mode::Public);
        QVERIFY(invite->accessKeyId.isEmpty());
        QCOMPARE(invite->urlFor("latest.json").toString(), QStringLiteral("https://files.example.com/share/") + invite->token + "/latest.json");
    }

    void systemRandomMakesDifferentValidTokens()
    {
        QSet<QString> seen;
        for (int i = 0; i < 20; ++i) {
            const QString id = randomId(systemRandomBytes);
            QVERIFY(isValidToken(id));
            QVERIFY(!seen.contains(id));
            seen.insert(id);
        }
        QCOMPARE(systemRandomBytes(16).size(), 16);
        QVERIFY(systemRandomBytes(16) != QByteArray(16, '\0'));
    }

    void brokenRandomSourceIsRefused()
    {
        QString error;
        QVERIFY(!newPackInvite(s3Config(), "x", [](int) { return QByteArray(); }, &error));
        QVERIFY(!error.isEmpty());
        QVERIFY(!newPackInvite(s3Config(), "x", [](int count) { return QByteArray(count, 'a'); }, &error));  // packId == token
    }

    void rotateKeepsThePackIdAndChangesTheToken()
    {
        const auto invite = newPackInvite(s3Config(), "Friends Pack", counting());
        QVERIFY(invite);
        const auto rotated = rotatedInvite(*invite, counting(100));
        QVERIFY(rotated);
        QCOMPARE(rotated->packId, invite->packId);  // the playtime ledger and ManagedPackID key on it
        QVERIFY(rotated->token != invite->token);
        QVERIFY(isValidToken(rotated->token));
        QCOMPARE(rotated->accessKeyId, invite->accessKeyId);
        QCOMPARE(rotated->bucket, invite->bucket);
        QVERIFY(parseInvite(inviteToString(*rotated)));
        // A source that hands out the old token again does not rotate anything.
        const QByteArray same = QByteArray::fromBase64(invite->token.toLatin1(), QByteArray::Base64UrlEncoding);
        QVERIFY(!rotatedInvite(*invite, [same](int) { return same; }));
    }

    void hostConfigValidation()
    {
        QVERIFY(validateHostConfig(s3Config()).isEmpty());
        QVERIFY(validateHostConfig(folderConfig("C:/www/packs")).isEmpty());

        HostConfig config = s3Config();
        config.readKeyId = config.writeKeyId;
        QVERIFY(!validateHostConfig(config).isEmpty());  // a write key must not become the members' key
        config = s3Config();
        config.readSecret = config.writeSecret;
        QVERIFY(!validateHostConfig(config).isEmpty());
        config = s3Config();
        config.readKeyId.clear();
        QVERIFY(!validateHostConfig(config).isEmpty());
        config = s3Config();
        config.endpoint = "http://0123456789abcdef.r2.cloudflarestorage.com";
        QVERIFY(!validateHostConfig(config).isEmpty());  // https only
        config = s3Config();
        config.bucket.clear();
        QVERIFY(!validateHostConfig(config).isEmpty());
        config = s3Config();
        config.prefix = "../x";
        QVERIFY(!validateHostConfig(config).isEmpty());
        config = folderConfig("relative/path");
        QVERIFY(!validateHostConfig(config).isEmpty());
        config = folderConfig("C:/www");
        config.endpoint = "http://files.example.com";
        QVERIFY(!validateHostConfig(config).isEmpty());
        for (const QString& problem : { validateHostConfig(s3Config()) })
            QVERIFY(!problem.contains("WRITE"));
        config = s3Config();
        config.readKeyId = config.writeKeyId;
        QVERIFY(!validateHostConfig(config).contains("WRITEKEYID"));  // the message never repeats a key
    }

    void hostConfigMessagesAreSentencesThatNameTheField()
    {
        // Each case: the broken config, a word of the field the message must name.
        struct Case {
            HostConfig config;
            const char* field;
        };
        std::vector<Case> cases;
        const auto add = [&cases](HostConfig config, const char* field) { cases.push_back({ std::move(config), field }); };
        HostConfig config = s3Config();
        config.bucket.clear();
        add(config, "bucket");
        config = s3Config();
        config.writeKeyId.clear();
        add(config, "Access Key ID");
        config = s3Config();
        config.writeSecret = "short";
        add(config, "Secret Access Key");
        config = s3Config();
        config.readKeyId.clear();
        add(config, "Read-Only Key ID");
        config = s3Config();
        config.readSecret.clear();
        add(config, "Read-Only Secret");
        config = s3Config();
        config.readKeyId = config.writeKeyId;
        add(config, "read-only key");
        config = s3Config();
        config.endpoint = "http://0123456789abcdef.r2.cloudflarestorage.com";
        add(config, "Endpoint");
        config = s3Config();
        config.prefix = "../x";
        add(config, "Prefix");
        add(folderConfig("relative/path"), "folder path");
        config = folderConfig("C:/www");
        config.endpoint = "http://files.example.com";
        add(config, "Public Address");
        config = folderConfig("C:/www");
        config.readKeyId = "READKEYID";
        add(config, "key");

        for (const Case& item : cases) {
            const QString message = validateHostConfig(item.config);
            QVERIFY2(!message.isEmpty(), item.field);
            QVERIFY2(message.at(0).isUpper(), qPrintable(message));  // a sentence, not a fragment
            QVERIFY2(message.endsWith(QLatin1Char('.')), qPrintable(message));
            QVERIFY2(message.contains(QLatin1String(item.field), Qt::CaseInsensitive), qPrintable(message));
            for (const QString& secret : item.config.secrets())
                QVERIFY2(!message.contains(secret), qPrintable(message));  // no key in a message, whatever the fault
        }
    }

    void keysOnTheHost()
    {
        QCOMPARE(s3Config().keyFor("TOKEN", "latest.json"), QStringLiteral("shared/TOKEN/latest.json"));
        HostConfig config = s3Config();
        config.prefix.clear();
        QCOMPARE(config.keyFor("TOKEN", "files/abc"), QStringLiteral("TOKEN/files/abc"));
    }

    // ---------------------------------------------------------------- owner role
    void ownerRoleBelongsToOneInstance()
    {
        QCOMPARE(standingOf("inst-a", QString("inst-a"), true), Standing::Owner);
        QCOMPARE(standingOf("inst-a", QString("inst-a"), false), Standing::Owner);
        // A copy of the owner's instance: same files, another id.
        QCOMPARE(standingOf("inst-copy", QString("inst-a"), true), Standing::Member);
        QCOMPARE(standingOf("inst-copy", QString("inst-a"), false), Standing::Unmanaged);
        // Another computer: no credentials at all.
        QCOMPARE(standingOf("inst-a", std::nullopt, true), Standing::Member);
        QCOMPARE(standingOf("inst-a", std::nullopt, false), Standing::Unmanaged);
        QCOMPARE(standingOf("", QString("inst-a"), true), Standing::Member);
    }

    void reconcileFixesOnlyWhatLies()
    {
        State owner;
        owner.packId = "pack-1";
        owner.role = Role::Owner;
        owner.revision = 4;
        owner.installed.insert("mods/a.jar", hashOf('1'));

        const auto copy = reconcileState(owner, Standing::Member);
        QVERIFY(copy);
        QCOMPARE(copy->role, Role::Member);
        QCOMPARE(copy->revision, 4);  // the 3-way base survives: the copy updates like any member
        QCOMPARE(copy->installed, owner.installed);
        QVERIFY(!reconcileState(owner, Standing::Owner));
        QVERIFY(!reconcileState(owner, Standing::Unmanaged));  // not ours: never rewritten

        State member = owner;
        member.role = Role::Member;
        QVERIFY(!reconcileState(member, Standing::Member));
        QCOMPARE(reconcileState(member, Standing::Owner)->role, Role::Owner);
    }

    // ---------------------------------------------------------------- credentials
    void credentialsRoundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(!QFile::exists(credentialsPath(dir.path())));
        QVERIFY(readCredentials(dir.path())->packs.isEmpty());  // no file is an empty set, not an error

        OwnerRecord record{ "inst-a", s3Config() };
        QVERIFY(writeOwnerRecord(dir.path(), "pack-1", record));
        OwnerRecord folder{ "inst-b", folderConfig("C:/www/packs") };
        QVERIFY(writeOwnerRecord(dir.path(), "pack-2", folder));

        const auto back = readCredentials(dir.path());
        QVERIFY(back);
        QCOMPARE(back->packs.size(), 2);
        const OwnerRecord one = back->packs.value("pack-1");
        QCOMPARE(one.instanceId, QStringLiteral("inst-a"));
        QCOMPARE(one.host.writeSecret, s3Config().writeSecret);
        QCOMPARE(one.host.readKeyId, s3Config().readKeyId);
        QCOMPARE(one.host.bucket, QStringLiteral("mypacks"));
        QCOMPARE(back->packs.value("pack-2").host.kind, HostConfig::Kind::Folder);
        QCOMPARE(back->packs.value("pack-2").host.folder, QStringLiteral("C:/www/packs"));

        // Replacing one record keeps the other.
        record.instanceId = "inst-z";
        QVERIFY(writeOwnerRecord(dir.path(), "pack-1", record));
        QCOMPARE(findOwnerRecord(dir.path(), "pack-1")->instanceId, QStringLiteral("inst-z"));
        QVERIFY(findOwnerRecord(dir.path(), "pack-2"));
        QVERIFY(!findOwnerRecord(dir.path(), "pack-3"));
    }

    void relayPublisherTokenRoundTrip()
    {
        QTemporaryDir dir;
        const QString worker = QStringLiteral("https://relay.example.workers.dev");
        const QString token = QStringLiteral("publisher-token-test-0123456789");
        const auto legacy = parseCredentials(R"({"formatVersion":1,"packs":{}})");
        QVERIFY(legacy);
        QVERIFY(legacy->publisherTokens.isEmpty());
        QVERIFY(legacy->lastPublisherWorker.isEmpty());
        QVERIFY(writePublisherToken(dir.path(), worker, token));
        QCOMPARE(findPublisherToken(dir.path(), worker).value(), token);
        QCOMPARE(findLastPublisherWorker(dir.path()).value(), worker);
        QCOMPARE(readCredentials(dir.path())->publisherTokens.size(), 1);
        QVERIFY(!writePublisherToken(dir.path(), QStringLiteral("http://relay.example.workers.dev"), token));
    }

    void aNewPackStartsOnTheSharedPacksServer()
    {
        QCOMPARE(defaultHostKind(std::nullopt), HostConfig::Kind::Relay);
        // an owner keeps the host of the pack that exists: Folder and S3 packs stay as they are
        QCOMPARE(defaultHostKind(HostConfig::Kind::S3), HostConfig::Kind::S3);
        QCOMPARE(defaultHostKind(HostConfig::Kind::Folder), HostConfig::Kind::Folder);
        QCOMPARE(defaultHostKind(HostConfig::Kind::Relay), HostConfig::Kind::Relay);
    }

    void publisherCodeRowShowsOnlyWithoutSavedAccess()
    {
        using Kind = HostConfig::Kind;
        // new pack, nothing saved: the code, and nothing of S3, Folder or a server address
        auto rows = hostRows(Kind::Relay, false, false, false);
        QVERIFY(rows.publisherCode);
        QVERIFY(!rows.newCodeButton);
        QVERIFY(!rows.otherHostPicker && !rows.folder && !rows.s3);
        // access saved: only Name, Changelog and the buttons; the code comes back only when asked for
        rows = hostRows(Kind::Relay, false, true, false);
        QVERIFY(!rows.publisherCode);
        QVERIFY(rows.newCodeButton);
        rows = hostRows(Kind::Relay, false, true, true);
        QVERIFY(rows.publisherCode);
        QVERIFY(!rows.newCodeButton);
        // an owner of a server pack never sees a code
        rows = hostRows(Kind::Relay, true, false, false);
        QVERIFY(!rows.publisherCode && !rows.newCodeButton);
    }

    void otherHostsShowFolderOrS3()
    {
        using Kind = HostConfig::Kind;
        auto rows = hostRows(Kind::Folder, false, false, false);
        QVERIFY(rows.otherHostPicker && rows.folder && !rows.s3 && !rows.publisherCode && !rows.newCodeButton);
        rows = hostRows(Kind::S3, false, true, false);
        QVERIFY(rows.otherHostPicker && rows.s3 && !rows.folder && !rows.publisherCode && !rows.newCodeButton);
        // an existing owner of a Folder or S3 pack gets the same rows (the dialog only disables them)
        rows = hostRows(Kind::S3, true, false, false);
        QVERIFY(rows.otherHostPicker && rows.s3 && !rows.folder);
    }

    void savedServerNeedsItsToken()
    {
        QTemporaryDir dir;
        const QString worker = QStringLiteral("https://relay.example.workers.dev");
        const QString other = QStringLiteral("https://other.example.workers.dev");
        const QString token = QStringLiteral("publisher-token-test-0123456789");
        QVERIFY(!findSavedPublisherWorker(dir.path()));  // nothing redeemed yet: the dialog asks for the code
        QVERIFY(writePublisherToken(dir.path(), worker, token));
        QCOMPARE(findSavedPublisherWorker(dir.path()).value(), worker);  // the address comes from the stored code, no field
        // the last server has no token (lost or never saved): the code is needed again
        Credentials credentials = *readCredentials(dir.path());
        credentials.lastPublisherWorker = other;
        QFile file(credentialsPath(dir.path()));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(credentialsToJson(credentials));
        file.close();
        QVERIFY(readCredentials(dir.path()));
        QVERIFY(!findSavedPublisherWorker(dir.path()));
    }

    void credentialsWriteIsAtomicAndLeavesNothingBehind()
    {
        QTemporaryDir dir;
        QVERIFY(writeOwnerRecord(dir.path(), "pack-1", { "inst-a", s3Config() }));
        QVERIFY(writeOwnerRecord(dir.path(), "pack-1", { "inst-b", s3Config() }));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files), QStringList{ "sharedpack-credentials.json" });  // no temporary file left
    }

#ifdef Q_OS_WIN
    void credentialsFileIsOnlyForTheCurrentUser()
    {
        QTemporaryDir dir;
        QVERIFY(writeOwnerRecord(dir.path(), "pack-1", { "inst-a", s3Config() }));
        QVERIFY(writeOwnerRecord(dir.path(), "pack-1", { "inst-b", s3Config() }));  // a replace keeps it too

        const std::wstring path = QDir::toNativeSeparators(credentialsPath(dir.path())).toStdWString();
        PACL acl = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        QVERIFY(GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl, nullptr, &descriptor) ==
                ERROR_SUCCESS);
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        QVERIFY(GetSecurityDescriptorControl(descriptor, &control, &revision));
        const int entries = acl ? acl->AceCount : -1;
        LocalFree(descriptor);
        QVERIFY(control & SE_DACL_PROTECTED);  // nothing is inherited from the data folder
        QCOMPARE(entries, 1);                  // the current user only
    }
#endif

    void damagedCredentialsAreNeverOverwritten()
    {
        QTemporaryDir dir;
        QVERIFY(writeOwnerRecord(dir.path(), "pack-1", { "inst-a", s3Config() }));
        const QString path = credentialsPath(dir.path());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write("{ \"formatVersion\": 1, \"packs\": { \"pack-1\": { \"kind\": \"s3\" } } }");  // a record the validator refuses
        file.close();

        QVERIFY(!readCredentials(dir.path()));
        QVERIFY(!writeOwnerRecord(dir.path(), "pack-2", { "inst-b", s3Config() }));
        QVERIFY(!findOwnerRecord(dir.path(), "pack-1"));
        QFile again(path);
        QVERIFY(again.open(QIODevice::ReadOnly));
        QVERIFY(again.readAll().contains("\"kind\": \"s3\" }"));  // untouched
    }

    void credentialsRefuseBadRecords()
    {
        QTemporaryDir dir;
        HostConfig config = s3Config();
        config.readKeyId = config.writeKeyId;
        QVERIFY(!writeOwnerRecord(dir.path(), "pack-1", { "inst-a", config }));
        QVERIFY(!writeOwnerRecord(dir.path(), "bad id", { "inst-a", s3Config() }));
        QVERIFY(!writeOwnerRecord(dir.path(), "pack-1", { "", s3Config() }));
        QVERIFY(!QFile::exists(credentialsPath(dir.path())));
        QVERIFY(!parseCredentials("not json"));
        QVERIFY(!parseCredentials("{\"formatVersion\": 2, \"packs\": {}}"));
    }

    void credentialSecretsNeverReachTheLog()
    {
        QTemporaryDir dir;
        const HostConfig config = s3Config();
        QVERIFY(writeOwnerRecord(dir.path(), "pack-1", { "inst-a", config }));
        // Loading a record registers its secrets: a URL or message that carries one comes out redacted.
        QVERIFY(findOwnerRecord(dir.path(), "pack-1"));
        for (const QString& secret : config.secrets()) {
            const QString line = QStringLiteral("PUT https://host/x?key=%1 failed: %1").arg(secret);
            const QString redacted = redactForLog(line);
            QVERIFY2(!redacted.contains(secret), "a secret is still in the log line");
        }
        // The validator's messages never contain a key.
        HostConfig bad = config;
        bad.readKeyId = bad.writeKeyId;
        const QString message = validateHostConfig(bad);
        for (const QString& secret : config.secrets())
            QVERIFY(!message.contains(secret));
    }

    // ---------------------------------------------------------------- scan and Modrinth lookup
    void scanHashesWhatWouldBePublished()
    {
        QTemporaryDir dir;
        const auto write = [&dir](const QString& path, const QByteArray& data) {
            QDir().mkpath(QFileInfo(dir.path() + "/" + path).absolutePath());
            QFile file(dir.path() + "/" + path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(data);
        };
        write("mods/a.jar", "AAAA");
        write("mods/.index/a.pw.toml", "meta");
        write("config/sub/c.toml", "x=1");
        write("options.txt", "fov:90");
        write("saves/world/level.dat", "world");
        write("logs/latest.log", "log");
        write(".fabric/processedMods/p.jar", "p");
        write("empty.dat", "");

        qint64 lastProgress = 0;
        const ScanResult result = scanGameFolder(dir.path(), nullptr, [&lastProgress](qint64 bytes) { lastProgress = bytes; });
        QVERIFY2(result, qPrintable(result.error));
        QStringList paths;
        for (const ScannedFile& file : result.files)
            paths.append(file.path);
        QCOMPARE(paths, (QStringList{ "config/sub/c.toml", "empty.dat", "mods/a.jar", "options.txt" }));
        const ScannedFile& jar = result.files.at(2);
        QCOMPARE(jar.size, qint64(4));
        QCOMPARE(jar.sha512, QString::fromLatin1(QCryptographicHash::hash("AAAA", QCryptographicHash::Sha512).toHex()));
        QCOMPARE(jar.sha256, QString::fromLatin1(QCryptographicHash::hash("AAAA", QCryptographicHash::Sha256).toHex()));
        QCOMPARE(QFileInfo(jar.absolute).fileName(), QStringLiteral("a.jar"));
        QCOMPARE(lastProgress, qint64(4 + 3 + 0 + 6));
        QVERIFY(result.skipped.join("\n").contains("saves/"));
        QVERIFY(result.skipped.join("\n").contains(".fabric/"));
        QVERIFY(result.skipped.join("\n").contains("mods/.index/"));

        // What was scanned builds a manifest without complaint.
        BuildInput input = baseInput();
        for (const ScannedFile& file : result.files)
            input.files.append(File{ file.path, file.size, file.sha512, {} });
        QVERIFY(buildManifest(input));
    }

    void scanCanBeCancelled()
    {
        QTemporaryDir dir;
        QFile file(dir.path() + "/a.bin");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QByteArray(100, 'x'));
        file.close();
        std::atomic_bool cancel{ true };
        const ScanResult result = scanGameFolder(dir.path(), &cancel);
        QVERIFY(!result);
        QVERIFY(result.cancelled);
        QVERIFY(!scanGameFolder(dir.path() + "/missing"));
    }

    void modrinthUrlsAreOnlyTakenForTheExactFile()
    {
        const QString h1(128, 'a'), h2(128, 'b'), h3(128, 'c'), h4(128, 'd');
        const QByteArray response =
            "{ \"" + h1.toLatin1() + "\": { \"files\": [ { \"hashes\": { \"sha512\": \"" + h4.toLatin1() +
            "\" }, \"url\": \"https://cdn.modrinth.com/data/X/versions/1/other.jar\" },"
            " { \"hashes\": { \"sha512\": \"" + h1.toLatin1() + "\" }, \"url\": \"https://cdn.modrinth.com/data/X/versions/1/mod.jar\" } ] },"
            " \"" + h2.toLatin1() + "\": { \"files\": [ { \"hashes\": { \"sha512\": \"" + h2.toLatin1() + "\" }, \"url\": \"https://evil.example.com/mod.jar\" } ] } }";
        const auto urls = parseModrinthUrls(response, { h1, h2, h3 });
        QCOMPARE(urls.size(), 1);
        QCOMPARE(urls.value(h1), QStringLiteral("https://cdn.modrinth.com/data/X/versions/1/mod.jar"));  // the file with OUR hash, not the first one
        QVERIFY(!urls.contains(h2));  // not a host a manifest may name
        QVERIFY(!urls.contains(h3));  // unknown to Modrinth: needs its own blob
        QVERIFY(parseModrinthUrls("not json", { h1 }).isEmpty());
    }};

QTEST_GUILESS_MAIN(SharedPackPublishTest)

#include "SharedPackPublish_test.moc"
