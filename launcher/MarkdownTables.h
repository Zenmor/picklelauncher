// SPDX-License-Identifier: GPL-3.0-only
/*
 *  picklelauncher - GFM pipe tables for markdownToHTML (pickle-descriptions)
 *
 *  The cmark library used by markdownToHTML has no table extension, so a pipe table comes out as one paragraph.
 *  This pre-pass turns pipe tables into HTML tables (the same shape cmark-gfm emits) before cmark runs.
 */

#pragma once

#include <QString>
#include <functional>

namespace MarkdownTables {

/** Renders the markdown of one table cell to inline HTML (no surrounding paragraph). */
using InlineRenderer = std::function<QString(const QString&)>;

/** Replaces every GFM pipe table in @p markdown by an HTML table. Tables inside fenced or indented code are left alone.
 *  Text without a table is returned unchanged.
 */
QString convert(const QString& markdown, const InlineRenderer& renderInline);

/** Same, with cell contents rendered by cmark (the options markdownToHTML uses). */
QString convert(const QString& markdown);

}  // namespace MarkdownTables
