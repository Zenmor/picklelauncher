// SPDX-License-Identifier: GPL-3.0-only

#include <QTest>

#include "MarkdownTables.h"

using namespace MarkdownTables;

namespace {

QString plain(const QString& text)
{
    return text;
}

QString html(const QStringList& lines)
{
    return lines.join('\n');
}

}  // namespace

class MarkdownTablesTest : public QObject {
    Q_OBJECT

   private slots:
    void basicTableWithAlignment()
    {
        const QString md = "before\n\n| A | B |\n|:--|--:|\n| 1 | 2 |\nafter";
        const QString expected = html({ "before", "", "", "<table>", "<thead>", "<tr>", "<th align=\"left\">A</th>",
                                        "<th align=\"right\">B</th>", "</tr>", "</thead>", "<tbody>", "<tr>", "<td align=\"left\">1</td>",
                                        "<td align=\"right\">2</td>", "</tr>", "</tbody>", "</table>", "", "after" });
        QCOMPARE(convert(md, plain), expected);
    }

    void headerOnlyTableHasNoBody()
    {
        const QString out = convert("| A |\n|---|", plain);
        QVERIFY(out.contains("<th>A</th>"));
        QVERIFY(!out.contains("<tbody>"));
    }

    void withoutOuterPipes()
    {
        const QString out = convert("A | B\n--|--\n1 | 2", plain);
        QVERIFY(out.contains("<th>A</th>"));
        QVERIFY(out.contains("<td>2</td>"));
    }

    void textWithoutTableIsUnchanged()
    {
        const QString md = "a | b\nnot a delimiter\n\n---\n\nplain text";
        QCOMPARE(convert(md, plain), md);
        QCOMPARE(convert("no pipes at all", plain), QString("no pipes at all"));
    }

    void columnCountMustMatch()
    {
        const QString md = "| A | B |\n|---|---|---|\n| 1 | 2 |";
        QCOMPARE(convert(md, plain), md);
    }

    void escapedPipeStaysInsideTheCell()
    {
        const QString out = convert("| A | B |\n|---|---|\n| x \\| y | z |", plain);
        QVERIFY(out.contains("<td>x | y</td>"));
        QVERIFY(out.contains("<td>z</td>"));
    }

    void shortRowsArePaddedAndLongRowsCut()
    {
        const QString out = convert("| A | B |\n|---|---|\n| 1 |\n| 1 | 2 | 3 |", plain);
        QCOMPARE(out.count("<td"), 4);
        QVERIFY(out.contains("<td></td>"));
        QVERIFY(!out.contains(">3<"));
    }

    void tableEndsAtBlankLine()
    {
        const QString out = convert("| A |\n|---|\n| 1 |\n\n| not a row |\ntext", plain);
        QCOMPARE(out.count("<table>"), 1);
        QVERIFY(out.endsWith("\n\n| not a row |\ntext"));
    }

    void codeIsLeftAlone()
    {
        const QString fenced = "```\n| A | B |\n|---|---|\n| 1 | 2 |\n```";
        QCOMPARE(convert(fenced, plain), fenced);
        const QString tilde = "~~~\n| A | B |\n|---|---|\n~~~";
        QCOMPARE(convert(tilde, plain), tilde);
        const QString indented = "    | A | B |\n    |---|---|\n    | 1 | 2 |";
        QCOMPARE(convert(indented, plain), indented);
        // a table after a closed fence is converted again
        QVERIFY(convert("```\nx\n```\n\n| A |\n|---|\n| 1 |", plain).contains("<table>"));
    }

    void crlfInput()
    {
        const QString out = convert("| A | B |\r\n|---|---|\r\n| 1 | 2 |\r\n", plain);
        QVERIFY(out.contains("<th>A</th>"));
        QVERIFY(out.contains("<td>2</td>"));
    }

    void cellsAreRendered()
    {
        auto upper = [](const QString& text) { return text.toUpper(); };
        QVERIFY(convert("| a |\n|---|\n| b |", upper).contains("<td>B</td>"));
    }

    void entityCullingCompatibilityTable()
    {
        // the table of EntityCulling on Modrinth (emoji replaced to keep the source ASCII)
        const QString md =
            "|   Minecraft   |        Loader         |     Status      | Version  |                Note                 |\n"
            "|---------------|-----------------------|-----------------|----------|-------------------------------------|\n"
            "| 1.19.4+       | Fabric/Forge/NeoForge | Supported       | Latest   |                                     |\n"
            "| 1.16.5-1.19.2 | Fabric/Forge          | Not supported   | Outdated | Might get new updates at some point |\n"
            "| b1.7.3        | Babric                | Not supported   | Outdated | No updates planned                  |\n";
        const QString out = convert(md, plain);
        QCOMPARE(out.count("<tr>"), 4);
        QCOMPARE(out.count("<th>"), 5);
        QCOMPARE(out.count("<td>"), 15);
        QVERIFY(out.contains("<td>Fabric/Forge/NeoForge</td>"));
        QVERIFY(out.contains("<td>Might get new updates at some point</td>"));
    }
};

QTEST_GUILESS_MAIN(MarkdownTablesTest)

#include "MarkdownTables_test.moc"
