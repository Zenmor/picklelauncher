// SPDX-License-Identifier: GPL-3.0-only

#include "MarkdownTables.h"

#include <cmark.h>

#include <QRegularExpression>
#include <QStringList>
#include <cstdlib>

namespace MarkdownTables {

namespace {

// Splits one table row into trimmed cells. A pipe separates cells unless it is escaped with a backslash.
QStringList splitRow(const QString& line)
{
    QString text = line.trimmed();
    if (text.startsWith('|'))
        text.remove(0, 1);
    if (text.endsWith('|') && !text.endsWith("\\|"))
        text.chop(1);

    QStringList cells;
    QString cell;
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '|') {
            cell += '|';
            ++i;
        } else if (text[i] == '|') {
            cells << cell.trimmed();
            cell.clear();
        } else {
            cell += text[i];
        }
    }
    cells << cell.trimmed();
    return cells;
}

// A delimiter row looks like "|:--|:-:|--:|". Fills the column alignments ("", "left", "center" or "right").
bool parseDelimiter(const QString& line, QStringList& aligns)
{
    static const QRegularExpression s_cell(R"(^:?-+:?$)");

    if (!line.contains('|'))
        return false;
    aligns.clear();
    for (const auto& cell : splitRow(line)) {
        if (!s_cell.match(cell).hasMatch())
            return false;
        const bool left = cell.startsWith(':');
        const bool right = cell.endsWith(':');
        aligns << (left && right ? "center" : left ? "left" : right ? "right" : "");
    }
    return true;
}

QString renderTable(const QStringList& header, const QStringList& aligns, const QStringList& rows, const InlineRenderer& renderInline)
{
    auto cell = [&](const char* tag, const QString& text, qsizetype column) {
        const QString align = aligns[column].isEmpty() ? QString() : QString(" align=\"%1\"").arg(aligns[column]);
        return QString("<%1%2>%3</%1>").arg(QLatin1String(tag), align, renderInline(text));
    };

    QStringList html{ "<table>", "<thead>", "<tr>" };
    for (qsizetype column = 0; column < header.size(); ++column)
        html << cell("th", header[column], column);
    html << "</tr>" << "</thead>";

    if (!rows.isEmpty()) {
        html << "<tbody>";
        for (const auto& row : rows) {
            const auto cells = splitRow(row);
            html << "<tr>";
            // like GFM: missing cells are empty, extra cells are dropped
            for (qsizetype column = 0; column < header.size(); ++column)
                html << cell("td", column < cells.size() ? cells[column] : QString(), column);
            html << "</tr>";
        }
        html << "</tbody>";
    }
    html << "</table>";
    return html.join('\n');
}

}  // namespace

QString convert(const QString& markdown, const InlineRenderer& renderInline)
{
    if (!markdown.contains('|'))
        return markdown;

    static const QRegularExpression s_fence(R"(^ {0,3}(`{3,}|~{3,}))");

    const QStringList lines = markdown.split('\n');
    QStringList out;
    QChar fence;  // the character of the open code fence, null outside of one
    for (qsizetype i = 0; i < lines.size(); ++i) {
        const QString& line = lines[i];

        if (const auto match = s_fence.match(line); match.hasMatch()) {
            const QChar ch = match.captured(1)[0];
            if (fence.isNull())
                fence = ch;
            else if (ch == fence)
                fence = QChar();
            out << line;
            continue;
        }

        QStringList aligns;
        const bool indentedCode = line.startsWith("    ") || line.startsWith('\t');
        if (fence.isNull() && !indentedCode && i + 1 < lines.size() && line.contains('|') && parseDelimiter(lines[i + 1], aligns)) {
            const auto header = splitRow(line);
            if (header.size() == aligns.size()) {
                qsizetype end = i + 2;
                while (end < lines.size() && !lines[end].trimmed().isEmpty() && lines[end].contains('|'))
                    ++end;
                // blank lines around the block: cmark ends an HTML block at the first blank line
                out << QString() << renderTable(header, aligns, lines.mid(i + 2, end - i - 2), renderInline) << QString();
                i = end - 1;
                continue;
            }
        }
        out << line;
    }
    return out.join('\n');
}

QString convert(const QString& markdown)
{
    return convert(markdown, [](const QString& text) {
        const QByteArray data = text.toUtf8();
        char* buffer = cmark_markdown_to_html(data.constData(), data.length(), CMARK_OPT_NOBREAKS | CMARK_OPT_UNSAFE);
        QString html = QString::fromUtf8(buffer).trimmed();
        free(buffer);

        // a cell is inline content: drop the paragraph cmark wraps around it
        if (html.startsWith("<p>") && html.endsWith("</p>"))
            html = html.mid(3, html.size() - 7);
        return html;
    });
}

}  // namespace MarkdownTables
