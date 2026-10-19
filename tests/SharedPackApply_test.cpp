// SPDX-License-Identifier: GPL-3.0-only

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <filesystem>

#include "sharedpack/SharedPackApply.h"
#include "sharedpack/SharedPackDisk.h"

using namespace SharedPack;

namespace {

QString shaOf(const QByteArray& data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha512).toHex());
}

bool writeFile(const QString& path, const QByteArray& data, const QDateTime& time = {})
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(data);
    if (time.isValid())
        file.setFileTime(time, QFileDevice::FileModificationTime);
    return true;
}

QByteArray readFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("<unreadable>");
}

const QDateTime LongAgo = QDateTime(QDate(2001, 1, 1), QTime(0, 0), Qt::UTC);

struct Fixture {
    QTemporaryDir root;
    Layout layout;
    State state;
    Manifest manifest;

    Fixture()
    {
        layout.gameDir = root.path() + "/inst/minecraft";
        layout.stateFile = root.path() + "/inst/sharedpack-state.json";
        layout.workDir = root.path() + "/inst/sharedpack-tmp";
        QDir().mkpath(layout.gameDir);
        state.packId = "pack-1";
        manifest.packId = "pack-1";
        manifest.name = "Friends Pack";
        manifest.revision = 1;
    }

    /// Adds a file to the manifest and its blob to the work dir (the blob gets an old mtime on purpose).
    void pack(const QString& path, const QByteArray& content, bool withBlob = true)
    {
        File file;
        file.path = path;
        file.size = content.size();
        file.sha512 = shaOf(content);
        manifest.files.append(file);
        if (withBlob)
            QVERIFY(writeFile(layout.blobPath(file.sha512), content, LongAgo));
    }

    QString abs(const QString& path) const { return layout.gameDir + '/' + path; }

    QStringList allPaths() const
    {
        QStringList paths;
        for (const File& file : manifest.files)
            paths << file.path;
        for (auto it = state.installed.cbegin(); it != state.installed.cend(); ++it)
            paths << it.key();
        return paths;
    }

    Plan plan() const { return planUpdate(state, manifest, scanDisk(layout.gameDir, allPaths())); }

    ApplyResult apply()
    {
        const Plan p = plan();
        const ApplyResult result = applyPlan(layout, state, p);
        if (result.ok) {
            const auto parsed = parseState(readFile(layout.stateFile));
            if (parsed)
                state = *parsed.value;
        }
        return result;
    }

    /// The next revision: the manifest is replaced, blobs of the old one stay in the work dir.
    void nextRevision()
    {
        manifest.files.clear();
        manifest.revision++;
    }
};

int countOf(const Plan& plan, ActionKind kind)
{
    int n = 0;
    for (const Action& action : plan.actions)
        n += action.kind == kind ? 1 : 0;
    return n;
}

}  // namespace

class SharedPackApplyTest : public QObject {
    Q_OBJECT

   private slots:
    void scanMissingFileDirectory()
    {
        Fixture f;
        QVERIFY(writeFile(f.abs("mods/a.jar"), "AAA"));
        QVERIFY(QDir().mkpath(f.abs("mods/dir.jar")));
        const DiskState disk = scanDisk(f.layout.gameDir, { "mods/a.jar", "mods/missing.jar", "mods/dir.jar", "MODS/A.JAR" });
        QCOMPARE(disk.size(), 2);  // the missing one is left out, the upper-case spelling is the same key
        QCOMPARE(disk.value("mods/a.jar").kind, DiskKind::File);
        QCOMPARE(disk.value("mods/a.jar").sha512, shaOf("AAA"));
        QCOMPARE(disk.value("mods/dir.jar").kind, DiskKind::Unsafe);
        QVERIFY(!disk.contains("mods/missing.jar"));
    }

    void scanFileBelowAFileIsUnsafe()
    {
        Fixture f;
        QVERIFY(writeFile(f.abs("mods"), "I am a file, not a folder"));
        const DiskState disk = scanDisk(f.layout.gameDir, { "mods/a.jar" });
        QCOMPARE(disk.value("mods/a.jar").kind, DiskKind::Unsafe);
    }

    void symlinkedFolderAndFileAreUnsafe()
    {
        Fixture f;
        QTemporaryDir outside;
        QVERIFY(writeFile(outside.path() + "/real.jar", "REAL"));
        std::error_code ec;
        std::filesystem::create_directory_symlink(outside.path().toStdWString(), f.abs("mods").toStdWString(), ec);
        if (ec)
            QSKIP(qPrintable("cannot create a symlink here (needs admin or developer mode): " + QString::fromStdString(ec.message())));
        QVERIFY(!ancestorsAreSafe(f.layout.gameDir, "mods/a.jar"));
        QCOMPARE(scanDisk(f.layout.gameDir, { "mods/real.jar" }).value("mods/real.jar").kind, DiskKind::Unsafe);

        // planned: skipped and reported, nothing lands outside
        f.pack("mods/a.jar", "NEW");
        const ApplyResult result = f.apply();
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(result.skipped, QStringList{ "mods/a.jar" });
        QVERIFY(!QFile::exists(outside.path() + "/a.jar"));
    }

    void junctionedFolderIsUnsafe()
    {
#ifdef Q_OS_WIN
        Fixture f;
        QTemporaryDir outside;
        QVERIFY(writeFile(outside.path() + "/real.jar", "REAL"));
        // a junction needs no privilege, unlike a symlink
        const int code = QProcess::execute(
            "cmd", { "/c", "mklink", "/J", QDir::toNativeSeparators(f.abs("mods")), QDir::toNativeSeparators(outside.path()) });
        if (code != 0)
            QSKIP("cannot create a junction here");
        QVERIFY(isLinkOrReparsePoint(f.abs("mods")));
        QVERIFY(!ancestorsAreSafe(f.layout.gameDir, "mods/a.jar"));
        QCOMPARE(scanDisk(f.layout.gameDir, { "mods/real.jar" }).value("mods/real.jar").kind, DiskKind::Unsafe);
        f.pack("mods/a.jar", "NEW");
        // planned: skipped and reported, nothing lands outside
        const ApplyResult result = f.apply();
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(result.skipped, QStringList{ "mods/a.jar" });
        QVERIFY(!QFile::exists(outside.path() + "/a.jar"));
        QCOMPARE(readFile(outside.path() + "/real.jar"), QByteArray("REAL"));
        // even a plan that insists on writing through the junction is refused by the applier itself
        Plan forced;
        forced.revision = 1;
        forced.actions.append(Action{ ActionKind::Write, "mods/a.jar", shaOf("NEW"), Why::NewFile });
        QVERIFY(!applyPlan(f.layout, State{}, forced).ok);
        QVERIFY(!QFile::exists(outside.path() + "/a.jar"));
#else
        QSKIP("junctions are Windows only");
#endif
    }

    void symlinkedFileIsNotTouched()
    {
        Fixture f;
        QTemporaryDir outside;
        QVERIFY(writeFile(outside.path() + "/real.jar", "REAL"));
        QVERIFY(QDir().mkpath(f.abs("mods")));
        std::error_code ec;
        std::filesystem::create_symlink(QString(outside.path() + "/real.jar").toStdWString(), f.abs("mods/a.jar").toStdWString(), ec);
        if (ec)
            QSKIP(qPrintable("cannot create a symlink here (needs admin or developer mode): " + QString::fromStdString(ec.message())));
        f.pack("mods/a.jar", "NEW");
        QCOMPARE(f.plan().actions.value(0).kind, ActionKind::Skip);
        f.apply();
        QCOMPARE(readFile(outside.path() + "/real.jar"), QByteArray("REAL"));
    }

    void firstInstallWritesFilesAndStateLast()
    {
        Fixture f;
        f.pack("mods/a.jar", "AAAA");
        f.pack("config/x/y.toml", "key=1");
        const ApplyResult result = f.apply();
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(readFile(f.abs("mods/a.jar")), QByteArray("AAAA"));
        QCOMPARE(readFile(f.abs("config/x/y.toml")), QByteArray("key=1"));
        QCOMPARE(result.written.size(), 2);
        QCOMPARE(f.state.revision, 1);
        QCOMPARE(f.state.installed.value("mods/a.jar"), shaOf("AAAA"));
        QVERIFY(!QFile::exists(f.layout.workDir + "/stage"));
    }

    void newModificationTimeNeverTheSources()
    {
        Fixture f;
        f.pack("mods/a.jar", "AAAA");
        QVERIFY(f.apply().ok);
        const QDateTime mtime = QFileInfo(f.abs("mods/a.jar")).lastModified();
        QVERIFY2(mtime > QDateTime::currentDateTime().addSecs(-120), qPrintable(mtime.toString()));
        QVERIFY(mtime.date().year() > 2001);
    }

    void replaceWithSameSizeGetsNewTimeAndContent()
    {
        Fixture f;
        f.pack("mods/a.jar", "AAAA");
        QVERIFY(f.apply().ok);
        {
            QFile file(f.abs("mods/a.jar"));
            QVERIFY(file.open(QIODevice::ReadWrite));
            file.setFileTime(LongAgo, QFileDevice::FileModificationTime);
        }
        f.nextRevision();
        f.pack("mods/a.jar", "BBBB");  // same size
        const ApplyResult result = f.apply();
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(readFile(f.abs("mods/a.jar")), QByteArray("BBBB"));
        QVERIFY(QFileInfo(f.abs("mods/a.jar")).lastModified().date().year() > 2001);
        QCOMPARE(f.state.revision, 2);
    }

    void removedFileIsDeletedAndEmptyFoldersGo()
    {
        Fixture f;
        f.pack("mods/a.jar", "AAAA");
        f.pack("config/deep/er/c.toml", "c");
        f.pack("config/other/d.toml", "d");
        QVERIFY(f.apply().ok);
        QVERIFY(writeFile(f.abs("config/other/mine.txt"), "member's own file"));

        f.nextRevision();
        f.pack("mods/a.jar", "AAAA");  // config files dropped from the pack
        const ApplyResult result = f.apply();
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(result.deleted.size(), 2);
        QVERIFY(!QFile::exists(f.abs("config/deep/er/c.toml")));
        QVERIFY(!QDir(f.abs("config/deep")).exists());           // the whole empty chain is gone
        QVERIFY(QFile::exists(f.abs("config/other/mine.txt")));  // a folder with a member's file stays
        QVERIFY(!QFile::exists(f.abs("config/other/d.toml")));
        QVERIFY(QFile::exists(f.abs("mods/a.jar")));
    }

    void memberEditedFileBecomesPackNewOnConflict()
    {
        Fixture f;
        f.pack("config/a.toml", "v1");
        QVERIFY(f.apply().ok);
        QVERIFY(writeFile(f.abs("config/a.toml"), "member edit"));
        f.nextRevision();
        f.pack("config/a.toml", "v2");
        const ApplyResult result = f.apply();
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(readFile(f.abs("config/a.toml")), QByteArray("member edit"));
        QCOMPARE(readFile(f.abs("config/a.toml.packnew")), QByteArray("v2"));
        QCOMPARE(result.packNew, QStringList{ "config/a.toml" });
    }

    void memberFilesAreNeverTouched()
    {
        Fixture f;
        QVERIFY(writeFile(f.abs("mods/own.jar"), "mine"));
        QVERIFY(writeFile(f.abs("saves/world/level.dat"), "world"));
        f.pack("mods/a.jar", "AAAA");
        QVERIFY(f.apply().ok);
        QCOMPARE(readFile(f.abs("mods/own.jar")), QByteArray("mine"));
        QCOMPARE(readFile(f.abs("saves/world/level.dat")), QByteArray("world"));
    }

    void reapplyIsIdempotent()
    {
        Fixture f;
        f.pack("mods/a.jar", "AAAA");
        f.pack("config/c.toml", "c");
        QVERIFY(f.apply().ok);
        const Plan again = f.plan();
        QCOMPARE(countOf(again, ActionKind::Write), 0);
        QCOMPARE(countOf(again, ActionKind::Delete), 0);
    }

    void crashBeforeStateConverges()
    {
        Fixture f;
        f.pack("mods/a.jar", "AAAA");
        f.pack("mods/b.jar", "BBBB");
        QVERIFY(f.apply().ok);

        // revision 2 drops a, changes b, adds c; "crash" after the files were done: put the old state back
        const QByteArray oldStateJson = readFile(f.layout.stateFile);
        const State oldState = f.state;
        f.nextRevision();
        f.pack("mods/b.jar", "bbbb");
        f.pack("mods/c.jar", "CCCC");
        QVERIFY(f.apply().ok);
        QVERIFY(writeFile(f.layout.stateFile, oldStateJson));
        f.state = oldState;

        // the files are already in place: planning again finds nothing to write, the run completes and saves the state
        const Plan retry = f.plan();
        QCOMPARE(countOf(retry, ActionKind::Write), 0);
        const ApplyResult result = f.apply();
        QVERIFY2(result.ok, qPrintable(result.error));
        QCOMPARE(f.state.revision, 2);
        QVERIFY(!QFile::exists(f.abs("mods/a.jar")));
        QCOMPARE(readFile(f.abs("mods/b.jar")), QByteArray("bbbb"));
        QCOMPARE(readFile(f.abs("mods/c.jar")), QByteArray("CCCC"));
    }

    void failureLeavesNoStateFile()
    {
        Fixture f;
        f.pack("mods/a.jar", "AAAA");
        f.pack("mods/b.jar", "BBBB", false);  // blob missing
        const ApplyResult result = f.apply();
        QVERIFY(!result.ok);
        QVERIFY2(result.error.contains("b.jar"), qPrintable(result.error));  // names the file
        QVERIFY(!QFile::exists(f.layout.stateFile));
    }

    void corruptBlobIsRefused()
    {
        Fixture f;
        f.pack("mods/a.jar", "AAAA");
        QVERIFY(writeFile(f.layout.blobPath(shaOf("AAAA")), "XXXX"));  // changed after it was verified
        const ApplyResult result = f.apply();
        QVERIFY(!result.ok);
        QVERIFY(!QFile::exists(f.abs("mods/a.jar")));
        QVERIFY(!QFile::exists(f.layout.stateFile));
    }

    void directoryWhereAFileBelongsIsSkippedAndRefusedIfForced()
    {
        Fixture f;
        QVERIFY(QDir().mkpath(f.abs("mods/a.jar")));
        f.pack("mods/a.jar", "AAAA");
        const Plan plan = f.plan();
        QCOMPARE(plan.actions.value(0).kind, ActionKind::Skip);
        QCOMPARE(plan.actions.value(0).why, Why::UnsafeOnDisk);

        Plan forced;
        forced.revision = 1;
        forced.actions.append(Action{ ActionKind::Write, "mods/a.jar", shaOf("AAAA"), Why::NewFile });
        QVERIFY(!applyPlan(f.layout, f.state, forced).ok);
        QVERIFY(QDir(f.abs("mods/a.jar")).exists());
    }

    void staleIndexEntryOfAReplacedJarIsRemoved()
    {
        Fixture f;
        QVERIFY(writeFile(f.abs("mods/a.jar"), "OLD"));
        QVERIFY(writeFile(f.abs("mods/.index/some-mod.pw.toml"), "name = \"Some Mod\"\nfilename = \"a.jar\"\nside = \"both\"\n"));
        QVERIFY(writeFile(f.abs("mods/.index/other.pw.toml"), "name = \"Other\"\nfilename = \"other.jar\"\n"));
        f.pack("mods/a.jar", "NEW");
        const ApplyResult result = f.apply();
        QVERIFY2(result.ok, qPrintable(result.error));
        QVERIFY(!QFile::exists(f.abs("mods/.index/some-mod.pw.toml")));
        QVERIFY(QFile::exists(f.abs("mods/.index/other.pw.toml")));
        QCOMPARE(result.indexRemoved, QStringList{ "mods/.index/some-mod.pw.toml" });
    }

    void staleIndexEntryOfARemovedJarIsRemoved()
    {
        Fixture f;
        f.pack("mods/a.jar", "AAAA");
        QVERIFY(f.apply().ok);
        QVERIFY(writeFile(f.abs("mods/.index/some-mod.pw.toml"), "filename = \"a.jar\"\n"));
        f.nextRevision();
        const ApplyResult result = f.apply();
        QVERIFY2(result.ok, qPrintable(result.error));
        QVERIFY(!QFile::exists(f.abs("mods/a.jar")));
        QVERIFY(!QFile::exists(f.abs("mods/.index/some-mod.pw.toml")));
    }

    void refusedPlanIsNotApplied()
    {
        Fixture f;
        f.state.role = Role::Owner;
        f.pack("mods/a.jar", "AAAA");
        const Plan plan = f.plan();
        QCOMPARE(plan.refusal, Plan::Refusal::OwnerRole);
        QVERIFY(!applyPlan(f.layout, f.state, plan).ok);
        QVERIFY(!QFile::exists(f.abs("mods/a.jar")));
    }

    void requiredSpaceCountsBlobsAndCopies()
    {
        Fixture f;
        f.pack("mods/a.jar", QByteArray(1000, 'a'));
        f.pack("mods/b.jar", QByteArray(500, 'b'));
        const qint64 needed = requiredFreeBytes(f.plan(), f.manifest);
        QVERIFY(needed >= 2 * 1500);
        QVERIFY(needed < 2 * 1500 + 128LL * 1024 * 1024);
    }
};

QTEST_GUILESS_MAIN(SharedPackApplyTest)

#include "SharedPackApply_test.moc"
