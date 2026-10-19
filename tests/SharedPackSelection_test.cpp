// SPDX-License-Identifier: GPL-3.0-only
//
// Publish dialog selection (pickle-sharedpack): the default ticks, what is remembered, how an unticked path reaches the scan and the manifest,
// what members do when the owner stops publishing something, and the folder sizes behind the file tree.

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <atomic>

#include "FolderSizeCache.h"
#include "sharedpack/SharedPackBuild.h"
#include "sharedpack/SharedPackManifest.h"
#include "sharedpack/SharedPackPath.h"
#include "sharedpack/SharedPackPlan.h"
#include "sharedpack/SharedPackScan.h"
#include "sharedpack/SharedPackSelection.h"
#include "sharedpack/SharedPackState.h"

using namespace SharedPack;

namespace {

QString sha(char c)
{
    return QString(128, QLatin1Char(c));
}

void write(const QString& root, const QString& relative, const QByteArray& content)
{
    const QString path = QDir(root).filePath(relative);
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(content);
}

State state(const QMap<QString, QString>& installed, Role role = Role::Member)
{
    State s;
    s.packId = "pack-1";
    s.role = role;
    s.revision = 1;
    s.installed = installed;
    return s;
}

Manifest manifest(const QList<File>& files, const QStringList& released = {})
{
    Manifest m;
    m.packId = "pack-1";
    m.name = "P";
    m.revision = 2;
    m.files = files;
    m.released = released;
    return m;
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

BuildInput baseInput()
{
    BuildInput input;
    input.packId = "pack-1";
    input.name = "Friends Pack";
    input.revision = 1;
    input.components = { { "net.minecraft", "1.21.1" } };
    return input;
}

File file(const QString& path, char hash)
{
    return File{ path, 10, sha(hash), {} };
}

QStringList paths(const QList<File>& files)
{
    QStringList result;
    for (const File& f : files)
        result << f.path;
    return result;
}

QStringList paths(const QList<ScannedFile>& files)
{
    QStringList result;
    for (const ScannedFile& f : files)
        result << f.path;
    return result;
}

}  // namespace

class SharedPackSelectionTest : public QObject {
    Q_OBJECT

   private slots:
    // ---------------------------------------------------------------- default selection
    void defaultsTickOnlyThePackFolders()
    {
        const QStringList top{ "mods", "config", "defaultconfigs", "kubejs", "scripts", "datapacks", "resourcepacks", "shaderpacks",
                               "rinku-libraries", "respawnrecap", "portable_blueprints", "ntgl", "options.txt" };
        const QStringList unticked = defaultUnticked(top);
        QCOMPARE(unticked, QStringList({ "rinku-libraries", "respawnrecap", "portable_blueprints", "ntgl", "options.txt" }));
        for (const QString& ticked : defaultTickedFolders())
            QVERIFY2(!unticked.contains(ticked), qPrintable(ticked));
        QCOMPARE(defaultTickedFolders().size(), 8);
    }

    void defaultsAreCaseInsensitive()
    {
        QCOMPARE(defaultUnticked({ "Mods", "CONFIG", "Other" }), QStringList({ "Other" }));
        QVERIFY(defaultUnticked({}).isEmpty());
    }

    void initialSelectionRules()
    {
        const QStringList top{ "mods", "cache", "newfolder" };
        // a new pack: the defaults
        QCOMPARE(initialUnticked(std::nullopt, top, false), QStringList({ "cache", "newfolder" }));
        // an already published pack without a saved choice keeps everything ticked
        QVERIFY(initialUnticked(std::nullopt, top, true).isEmpty());
        // saved choices stay as they are; a folder that did not exist then follows the defaults
        const StoredSelection stored{ { "mods/extra.jar" }, { "mods", "cache" } };
        const QStringList result = initialUnticked(stored, top, false);
        QVERIFY(result.contains("mods/extra.jar"));
        QVERIFY(!result.contains("cache"));  // the owner had seen it and left it ticked
        QVERIFY(result.contains("newfolder"));
        QVERIFY(!result.contains("mods"));
    }

    // ---------------------------------------------------------------- persistence
    void selectionRoundTrip()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(!readSelection(dir.path()));
        const StoredSelection saved{ { "rinku-libraries", "config/secret.toml" }, { "mods", "config", "rinku-libraries" } };
        QVERIFY(writeSelection(dir.path(), saved));
        QVERIFY(QFile::exists(selectionPath(dir.path())));
        const auto loaded = readSelection(dir.path());
        QVERIFY(loaded);
        QCOMPARE(loaded->unticked, QStringList({ "config/secret.toml", "rinku-libraries" }));  // stored sorted: a stable file
        QCOMPARE(loaded->known, QStringList({ "config", "mods", "rinku-libraries" }));
    }

    void damagedOrUnsafeSelectionIsSafe()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        write(dir.path(), "sharedpack-selection.json", "{ not json");
        QVERIFY(!readSelection(dir.path()));  // the defaults apply: publishes less, not more

        QJsonObject object{ { "unticked", QJsonArray{ "../evil", "ok/path", "C:/abs" } }, { "known", QJsonArray{ "mods" } } };
        QFile::remove(selectionPath(dir.path()));
        write(dir.path(), "sharedpack-selection.json", QJsonDocument(object).toJson());
        const auto loaded = readSelection(dir.path());
        if (loaded) {
            QVERIFY(!loaded->unticked.contains("../evil"));
            QVERIFY(!loaded->unticked.contains("C:/abs"));
        }
    }

    // ---------------------------------------------------------------- path set
    void pathSetCoversBelowAndIgnoresCase()
    {
        const PathSet set({ "rinku-libraries", "config/a.toml" });
        QVERIFY(set.covers("rinku-libraries"));
        QVERIFY(set.covers("Rinku-Libraries/x/y.dll"));
        QVERIFY(set.covers("config/a.toml"));
        QVERIFY(!set.covers("config/b.toml"));
        QVERIFY(!set.covers("rinku-librariesX/a"));  // a name prefix is not a folder
        QVERIFY(!PathSet().covers("mods/a.jar"));
        QVERIFY(PathSet().isEmpty());
    }

    // ---------------------------------------------------------------- the filter reaches the scan
    void scanSkipsUntickedWithoutEnteringThem()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        write(dir.path(), "mods/a.jar", "aaa");
        write(dir.path(), "config/x.toml", "x");
        write(dir.path(), "config/y.toml", "y");
        write(dir.path(), "respawnrecap/big/data.bin", "dll");
        write(dir.path(), "rinku-libraries/big/libcef.dll", "dll");
        write(dir.path(), "options.txt", "o");
        write(dir.path(), "saves/w/level.dat", "s");

        const PathSet unticked({ "respawnrecap", "config/x.toml", "saves" });
        const ScanResult result = scanGameFolder(dir.path(), nullptr, {}, &unticked);
        QVERIFY2(result, qPrintable(result.error));
        const QStringList found = paths(result.files);
        QVERIFY(found.contains("mods/a.jar"));
        QVERIFY(found.contains("config/y.toml"));
        QVERIFY(found.contains("options.txt"));
        QVERIFY(!found.contains("config/x.toml"));
        QVERIFY(!found.contains("respawnrecap/big/data.bin"));
        QVERIFY(!found.contains("rinku-libraries/big/libcef.dll"));  // the policy stopgap: refused whether ticked or not
        QVERIFY(result.unticked.contains("respawnrecap"));           // listed once, not its contents
        QVERIFY(!result.unticked.contains("respawnrecap/big/data.bin"));
        QVERIFY(!result.unticked.contains("rinku-libraries"));       // refused by the policy first: not "released"
        QVERIFY(result.unticked.contains("config/x.toml"));
    }

    void scanWithoutSelectionIsUnchanged()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        write(dir.path(), "mods/a.jar", "aaa");
        write(dir.path(), "config/x.toml", "x");
        const ScanResult result = scanGameFolder(dir.path());
        QVERIFY(result);
        QCOMPARE(paths(result.files), QStringList({ "config/x.toml", "mods/a.jar" }));
        QVERIFY(result.unticked.isEmpty());
    }

    void tickedPathThePolicyRefusesStaysRefused()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        write(dir.path(), "saves/w/level.dat", "s");
        write(dir.path(), "logs/latest.log", "l");
        write(dir.path(), "mods/a.jar", "a");
        const PathSet nothingUnticked;
        const ScanResult result = scanGameFolder(dir.path(), nullptr, {}, &nothingUnticked);
        QVERIFY(result);
        QCOMPARE(paths(result.files), QStringList({ "mods/a.jar" }));
    }

    // ---------------------------------------------------------------- an unticked file never enters the manifest
    void untickedFileNeverEntersManifest()
    {
        BuildInput input = baseInput();
        input.files = { file("mods/a.jar", '1'), file("config/x.toml", '2'), file("rinku-libraries/libcef.dll", '3') };
        input.unticked = { "config", "rinku-libraries" };
        input.released = { "config", "rinku-libraries" };
        const BuildResult result = buildManifest(input);
        QVERIFY2(result, qPrintable(result.error));
        QCOMPARE(paths(result.manifest->files), QStringList({ "mods/a.jar" }));
        QCOMPARE(result.manifest->released, QStringList({ "config", "rinku-libraries" }));
    }

    void tickedFileThePolicyExcludesStaysExcluded()
    {
        BuildInput input = baseInput();
        input.files = { file("mods/a.jar", '1'), file("saves/w/level.dat", '2'), file("logs/latest.log", '3') };
        const BuildResult result = buildManifest(input);
        QVERIFY(result);
        QCOMPARE(paths(result.manifest->files), QStringList({ "mods/a.jar" }));
    }

    void releasedSurvivesTheJsonRoundTrip()
    {
        BuildInput input = baseInput();
        input.files = { file("mods/a.jar", '1') };
        input.released = { "rinku-libraries", "config/secret.toml" };
        const BuildResult built = buildManifest(input);
        QVERIFY(built);
        const auto parsed = parseManifest(manifestToJson(*built.manifest));
        QVERIFY2(parsed.value.has_value(), "manifest did not parse");
        QCOMPARE(parsed.value->released, QStringList({ "config/secret.toml", "rinku-libraries" }));  // written sorted
    }

    void manifestWithoutReleasedStillParses()
    {
        BuildInput input = baseInput();
        input.files = { file("mods/a.jar", '1') };
        const auto parsed = parseManifest(manifestToJson(*buildManifest(input).manifest));
        QVERIFY(parsed.value.has_value());
        QVERIFY(parsed.value->released.isEmpty());
    }

    void unsafeReleasedPathIsRejected()
    {
        QJsonObject object = QJsonDocument::fromJson(manifestToJson(manifest({ file("mods/a.jar", 'a') }))).object();
        object["released"] = QJsonArray{ "../victim" };
        QVERIFY(!parseManifest(QJsonDocument(object).toJson(QJsonDocument::Compact)).value.has_value());
    }

    // ---------------------------------------------------------------- what members do (the reason for `released`)
    void removedFileIsDeletedWhenNotReleased()
    {
        const auto plan = planUpdate(state({ { "config/a.toml", sha('a') } }), manifest({}), disk({ { "config/a.toml", sha('a') } }));
        const Action* action = find(plan, "config/a.toml");
        QVERIFY(action);
        QCOMPARE(action->kind, ActionKind::Delete);
        QCOMPARE(action->why, Why::PackRemoved);
    }

    void releasedFileIsKeptAndNoLongerTracked()
    {
        const auto plan = planUpdate(state({ { "config/a.toml", sha('a') }, { "config/sub/b.toml", sha('b') }, { "mods/gone.jar", sha('c') } }),
                                     manifest({}, { "config" }), disk({ { "config/a.toml", sha('a') }, { "config/sub/b.toml", sha('b') },
                                                                          { "mods/gone.jar", sha('c') } }));
        QVERIFY(plan);
        for (const QString& path : { QStringLiteral("config/a.toml"), QStringLiteral("config/sub/b.toml") }) {
            const Action* action = find(plan, path);
            QVERIFY2(action, qPrintable(path));
            QCOMPARE(action->kind, ActionKind::Keep);
            QCOMPARE(action->why, Why::Released);
            QVERIFY(!plan.installed.contains(path));
        }
        // a file that left the pack because the owner deleted it (not released) is still removed
        const Action* gone = find(plan, "mods/gone.jar");
        QVERIFY(gone);
        QCOMPARE(gone->kind, ActionKind::Delete);
    }

    void releaseOnlyAppliesToRemovedPaths()
    {
        // still in the pack: released has no say, the file is updated as always
        const auto plan = planUpdate(state({ { "config/a.toml", sha('a') } }), manifest({ file("config/a.toml", 'b') }, { "config" }),
                                     disk({ { "config/a.toml", sha('a') } }));
        const Action* action = find(plan, "config/a.toml");
        QVERIFY(action);
        QCOMPARE(action->kind, ActionKind::Write);
        QCOMPARE(plan.installed.value("config/a.toml"), sha('b'));
    }

    // ---------------------------------------------------------------- folder sizes
    void computeAddsUpFolders()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        write(dir.path(), "a/one.bin", QByteArray(100, 'x'));
        write(dir.path(), "a/b/two.bin", QByteArray(50, 'x'));
        write(dir.path(), "a/b/c/three.bin", QByteArray(7, 'x'));
        write(dir.path(), "top.bin", QByteArray(3, 'x'));
        QVERIFY(QDir(dir.path()).mkpath("empty"));

        const std::atomic_bool cancel{ false };
        const FolderSizes::Sizes sizes = FolderSizes::compute(dir.path(), cancel);
        QCOMPARE(sizes.value("a/b/c"), qint64(7));
        QCOMPARE(sizes.value("a/b"), qint64(57));
        QCOMPARE(sizes.value("a"), qint64(157));
        QCOMPARE(sizes.value("empty", -1), qint64(0));
        QCOMPARE(sizes.value(""), qint64(160));
    }

    void computeStoppedWalkLeavesNoWrongNumbers()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        write(dir.path(), "a/one.bin", QByteArray(100, 'x'));
        const std::atomic_bool cancel{ true };
        const FolderSizes::Sizes sizes = FolderSizes::compute(dir.path(), cancel);
        // whatever it recorded must be complete: a stopped walk never reports a folder that was only partly read
        QVERIFY(!sizes.contains("a") || sizes.value("a") == 100);
        QVERIFY(!sizes.contains("") || sizes.value("") == 100);
    }

    void displayTextShowsDotsUntilCounted()
    {
        QCOMPARE(FolderSizes::displayText(std::nullopt, false), QStringLiteral("..."));
        QVERIFY(FolderSizes::displayText(std::nullopt, true).isEmpty());  // a folder the walk could not count (a link)
        QVERIFY(!FolderSizes::displayText(1234, true).isEmpty());
        QVERIFY(!FolderSizes::displayText(1234, false).contains("..."));
    }

    void sortOrderBySize()
    {
        using FolderSizes::lessBySize;
        using FolderSizes::SortKey;
        const SortKey small{ 10, true, "b" }, big{ 500, true, "a" }, unknown{ 0, false, "c" };
        QVERIFY(lessBySize(small, big, true));
        QVERIFY(!lessBySize(big, small, true));
        QVERIFY(lessBySize(small, unknown, true));  // not counted yet counts as the biggest
        QVERIFY(!lessBySize(unknown, small, true));
        QVERIFY(!lessBySize(unknown, unknown, true));

        // equal sizes: by name, always ascending; a descending sort calls with swapped arguments
        const SortKey a{ 5, true, "a" }, b{ 5, true, "B" };
        QVERIFY(lessBySize(a, b, true));
        QVERIFY(!lessBySize(b, a, true));
        QVERIFY(lessBySize(b, a, false));
        QVERIFY(!lessBySize(a, b, false));
        QVERIFY(!lessBySize(a, a, true));
    }

    void cacheFillsInTheBackgroundAndNeverBlocks()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        write(dir.path(), "a/one.bin", QByteArray(100, 'x'));
        write(dir.path(), "a/b/two.bin", QByteArray(50, 'x'));

        FolderSizeCache cache;
        QVERIFY(!cache.size("a"));  // nothing is counted before start()
        QSignalSpy changed(&cache, &FolderSizeCache::changed);
        cache.start(dir.path());
        QTRY_VERIFY_WITH_TIMEOUT(cache.finished(), 10000);
        QVERIFY(changed.count() >= 1);
        QCOMPARE(cache.size("a").value_or(-1), qint64(150));
        QCOMPARE(cache.size("a/b").value_or(-1), qint64(50));
        QCOMPARE(cache.size("").value_or(-1), qint64(150));
        QVERIFY(!cache.size("missing"));
    }

    void cancelledCacheIsNotFinishedAndDestructsQuickly()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (int i = 0; i < 20; ++i)
            write(dir.path(), QStringLiteral("d%1/f.bin").arg(i), QByteArray(10, 'x'));
        FolderSizeCache cache;
        cache.start(dir.path());
        cache.cancel();
        QVERIFY(!cache.finished());
        // late results of the stopped walk are ignored: processing queued events must not flip it to finished
        QTest::qWait(100);
        QVERIFY(!cache.finished());
    }

    void restartForgetsOldSizes()
    {
        QTemporaryDir first, second;
        QVERIFY(first.isValid() && second.isValid());
        write(first.path(), "a/x.bin", QByteArray(10, 'x'));
        write(second.path(), "b/y.bin", QByteArray(20, 'x'));
        FolderSizeCache cache;
        cache.start(first.path());
        QTRY_VERIFY_WITH_TIMEOUT(cache.finished(), 10000);
        QVERIFY(cache.size("a"));
        cache.start(second.path());
        QVERIFY(!cache.size("a"));
        QTRY_VERIFY_WITH_TIMEOUT(cache.finished(), 10000);
        QCOMPARE(cache.size("b").value_or(-1), qint64(20));
    }
};

QTEST_GUILESS_MAIN(SharedPackSelectionTest)

#include "SharedPackSelection_test.moc"
