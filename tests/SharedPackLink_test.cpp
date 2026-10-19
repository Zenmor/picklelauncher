// SPDX-License-Identifier: GPL-3.0-only

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

#include "sharedpack/SharedPackLink.h"

using namespace SharedPack;

namespace {

Invite invite()
{
    Invite invite;
    invite.packId = "pack-1";
    invite.name = "Friends Pack";
    invite.endpoint = "https://files.example.com/share";
    invite.token = "AbCdEfGhIjKlMnOpQrStUv";
    return invite;
}

bool write(const QString& path, const QByteArray& data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

}  // namespace

class SharedPackLinkTest : public QObject {
    Q_OBJECT

   private slots:
    void layoutPaths()
    {
        const Layout layout = layoutFor("C:/inst/", "C:/inst/minecraft");
        QCOMPARE(layout.gameDir, QStringLiteral("C:/inst/minecraft"));
        QCOMPARE(layout.stateFile, QStringLiteral("C:/inst/sharedpack-state.json"));
        QCOMPARE(layout.workDir, QStringLiteral("C:/inst/sharedpack-tmp"));
        QCOMPARE(layout.blobPath("abc"), QStringLiteral("C:/inst/sharedpack-tmp/blobs/abc"));
    }

    void linkRoundTrip()
    {
        QTemporaryDir dir;
        Link link;
        link.invite = invite();
        link.autoUpdate = true;
        QVERIFY(writeLink(dir.path(), link));
        const auto back = readLink(dir.path());
        QVERIFY(back);
        QVERIFY(back->autoUpdate);
        QCOMPARE(back->invite.packId, QStringLiteral("pack-1"));
        QCOMPARE(inviteToString(back->invite), inviteToString(link.invite));
    }

    void noLinkOrDamagedLink()
    {
        QTemporaryDir dir;
        QVERIFY(!readLink(dir.path()));
        QVERIFY(write(linkPath(dir.path()), "not json"));
        QVERIFY(!readLink(dir.path()));
        QVERIFY(write(linkPath(dir.path()), QJsonDocument(QJsonObject{ { "invite", "picklepack:1:garbage" } }).toJson()));
        QVERIFY(!readLink(dir.path()));  // the invite inside is validated again
        QVERIFY(write(linkPath(dir.path()), QByteArray(100000, 'x')));
        QVERIFY(!readLink(dir.path()));
    }

    void autoUpdateDefaultsToOff()
    {
        QTemporaryDir dir;
        Link link;
        link.invite = invite();
        QVERIFY(writeLink(dir.path(), link));
        QVERIFY(!readLink(dir.path())->autoUpdate);
    }

    void freshStateWhenThereIsNoFile()
    {
        QTemporaryDir dir;
        const Layout layout = layoutFor(dir.path(), dir.path() + "/minecraft");
        const auto state = readStateOrFresh(layout, "pack-1");
        QVERIFY(state);
        QCOMPARE(state->revision, 0);
        QCOMPARE(state->packId, QStringLiteral("pack-1"));
        QCOMPARE(static_cast<int>(state->role), static_cast<int>(Role::Member));
    }

    void existingStateIsReadAndChecked()
    {
        QTemporaryDir dir;
        const Layout layout = layoutFor(dir.path(), dir.path() + "/minecraft");
        State saved;
        saved.packId = "pack-1";
        saved.revision = 4;
        QVERIFY(write(layout.stateFile, stateToJson(saved)));
        QCOMPARE(readStateOrFresh(layout, "pack-1")->revision, 4);
        QVERIFY(!readStateOrFresh(layout, "pack-2"));  // another pack: nothing is guessed
        QVERIFY(write(layout.stateFile, "{broken"));
        QVERIFY(!readStateOrFresh(layout, "pack-1"));  // damaged: not treated as "fresh", which would overwrite files
    }

    void writeStateRoundTrip()
    {
        QTemporaryDir dir;
        const Layout layout = layoutFor(dir.path(), dir.path() + "/minecraft");
        State state;
        state.packId = "pack-1";
        state.role = Role::Owner;
        state.revision = 7;
        state.installed.insert("mods/a.jar", QString(128, 'a'));
        QVERIFY(writeState(layout, state));
        const auto back = readStateOrFresh(layout, "pack-1");
        QVERIFY(back);
        QCOMPARE(back->role, Role::Owner);
        QCOMPARE(back->revision, 7);
        QCOMPARE(back->installed, state.installed);
        state.role = Role::Member;
        QVERIFY(writeState(layout, state));  // replaces atomically
        QCOMPARE(readStateOrFresh(layout, "pack-1")->role, Role::Member);
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files), QStringList{ "sharedpack-state.json" });
    }};

QTEST_GUILESS_MAIN(SharedPackLinkTest)

#include "SharedPackLink_test.moc"
