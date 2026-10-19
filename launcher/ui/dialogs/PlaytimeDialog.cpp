// SPDX-License-Identifier: GPL-3.0-only

#include "PlaytimeDialog.h"

#include <QDateTime>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QScrollArea>
#include <QStyleOptionProgressBar>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QVBoxLayout>

#include "Application.h"
#include "InstanceList.h"
#include "MMCTime.h"
#include "PlaytimeDetails.h"
#include "PlaytimeDetailsCollector.h"

namespace {

enum Column { NameColumn, TimeColumn, ShareColumn, BarColumn, ColumnCount };

constexpr int BarSteps = 1000;
constexpr int BarWidth = 120;

QString duration(qint64 seconds)
{
    return Time::prettifyDuration(seconds, APPLICATION->settings()->get("ShowGameTimeWithoutDays").toBool());
}

QTableWidgetItem* textItem(const QString& text, Qt::Alignment alignment = Qt::AlignLeft)
{
    auto* item = new QTableWidgetItem(text);
    item->setTextAlignment(alignment | Qt::AlignVCenter);
    return item;
}

// Pack and instance names come from outside, so everything that is not a fixed string is shown as plain text, never as rich text.
QLabel* plainLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(parent);
    label->setTextFormat(Qt::PlainText);
    label->setText(text);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

// Draws the share of a row (0 to BarSteps, stored in the item) as a small progress bar, so a long list needs no widget per row.
class BarDelegate : public QStyledItemDelegate {
   public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        // the cell background first, so a selected row looks the same in every column
        QStyleOptionViewItem item = option;
        initStyleOption(&item, index);
        option.widget->style()->drawPrimitive(QStyle::PE_PanelItemViewItem, &item, painter, option.widget);

        QStyleOptionProgressBar bar;
        bar.initFrom(option.widget);
        bar.rect = option.rect.adjusted(4, 7, -4, -7);
        bar.minimum = 0;
        bar.maximum = BarSteps;
        bar.progress = index.data(Qt::UserRole).toInt();
        bar.textVisible = false;
        option.widget->style()->drawControl(QStyle::CE_ProgressBar, &bar, painter, option.widget);
    }
};

}  // namespace

PlaytimeDialog::PlaytimeDialog(const PlaytimeBreakdown::Result& breakdown, QWidget* parent) : QDialog(parent), m_rows(breakdown.rows)
{
    setWindowTitle(tr("Playtime"));
    resize(640, 680);

    auto* total = new QLabel(tr("Total playtime: %1").arg(duration(breakdown.totalSeconds)), this);

    auto* table = new QTableWidget(static_cast<int>(breakdown.rows.size()), ColumnCount, this);
    table->setHorizontalHeaderLabels({ tr("Name"), tr("Time"), tr("Share"), QString() });
    table->setItemDelegateForColumn(BarColumn, new BarDelegate(table));
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setShowGrid(false);
    table->verticalHeader()->hide();
    auto* header = table->horizontalHeader();
    header->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
    header->setSectionResizeMode(TimeColumn, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(ShareColumn, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(BarColumn, QHeaderView::Fixed);
    header->resizeSection(BarColumn, BarWidth);

    for (int i = 0; i < breakdown.rows.size(); ++i) {
        const auto& row = breakdown.rows[i];

        const bool deletedCustom = row.key == PlaytimeBreakdown::DeletedCustomKey;
        auto* name = textItem(deletedCustom ? tr("Deleted custom instances") : row.name);
        if (deletedCustom)
            name->setToolTip(tr("The time of deleted instances that were not part of a modpack cannot be split by name."));
        table->setItem(i, NameColumn, name);

        table->setItem(i, TimeColumn, textItem(duration(row.seconds), Qt::AlignRight));
        table->setItem(i, ShareColumn, textItem(QLocale().toString(row.percent, 'f', 1) + '%', Qt::AlignRight));

        auto* bar = new QTableWidgetItem;
        bar->setData(Qt::UserRole, qRound(row.percent * BarSteps / 100.0));
        table->setItem(i, BarColumn, bar);

        // there is nothing to show about deleted custom instances, so the row cannot be selected
        if (deletedCustom) {
            for (int column = 0; column < ColumnCount; ++column)
                table->item(i, column)->setFlags(table->item(i, column)->flags() & ~Qt::ItemIsSelectable);
        }
    }

    m_pane = new QScrollArea(this);
    m_pane->setWidgetResizable(true);
    m_pane->setMinimumHeight(180);
    showHint();

    connect(table, &QTableWidget::itemSelectionChanged, this, [this, table] {
        const auto selected = table->selectionModel()->selectedRows();
        if (selected.isEmpty())
            showHint();
        else
            showDetails(selected.first().row());
    });

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(total);
    layout->addWidget(table, 2);
    layout->addWidget(m_pane, 3);
    layout->addWidget(buttons);
}

void PlaytimeDialog::showHint()
{
    // setWidget() deletes the previous content
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->addWidget(plainLabel(tr("Click a row to see details."), content));
    layout->addStretch();
    m_pane->setWidget(content);
}

void PlaytimeDialog::showDetails(int rowIndex)
{
    if (rowIndex < 0 || rowIndex >= m_rows.size())
        return showHint();
    const auto& row = m_rows[rowIndex];

    // reads the instances of the clicked row only, and only reads (see PlaytimeDetailsCollector.h)
    const auto details = PlaytimeDetails::collect(*APPLICATION->instances(), row.key, row.name);
    using Moment = PlaytimeDetails::Moment;

    const auto unknown = tr("unknown");
    const auto momentText = [this](const Moment& moment) {
        switch (moment.state) {
            case Moment::State::Known: {
                const auto date = QLocale().toString(QDateTime::fromMSecsSinceEpoch(moment.ms), QLocale::ShortFormat);
                return moment.estimated ? tr("%1 (estimated)").arg(date) : date;
            }
            case Moment::State::NotRecorded:
                return tr("not recorded");
            case Moment::State::NeverPlayed:
                break;
        }
        return tr("never played");
    };

    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);

    auto* title = plainLabel(details.name, content);
    auto titleFont = title->font();
    titleFont.setBold(true);
    title->setFont(titleFont);
    layout->addWidget(title);

    auto* summary = new QFormLayout;
    summary->setLabelAlignment(Qt::AlignLeft);
    if (details.isPack)
        summary->addRow(tr("Source"), plainLabel(details.sourceName, content));
    if (!details.instances.isEmpty()) {
        summary->addRow(tr("First played"), plainLabel(momentText(details.firstPlayed), content));
        summary->addRow(tr("Last played"), plainLabel(momentText(details.lastPlayed), content));
    }
    layout->addLayout(summary);

    if (details.isPack && details.deletedSeconds > 0)
        layout->addWidget(plainLabel(tr("Deleted instances of this pack: %1").arg(duration(details.deletedSeconds)), content));
    if (details.isPack && details.instances.isEmpty())
        layout->addWidget(plainLabel(tr("All instances of this pack were deleted."), content));

    for (const auto& block : details.instances) {
        auto* line = new QFrame(content);
        line->setFrameShape(QFrame::HLine);
        line->setFrameShadow(QFrame::Sunken);
        layout->addWidget(line);

        auto* form = new QFormLayout;
        form->setLabelAlignment(Qt::AlignLeft);
        if (details.isPack) {
            form->addRow(tr("Instance"), plainLabel(block.name, content));
            form->addRow(tr("Pack Version"), plainLabel(block.packVersion.isEmpty() ? unknown : block.packVersion, content));
        }
        form->addRow(tr("Minecraft Version"), plainLabel(block.minecraftVersion.isEmpty() ? unknown : block.minecraftVersion, content));

        QString loader = unknown;  // without the Minecraft version the components could not be read, so the loader is not known either
        if (!block.minecraftVersion.isEmpty())
            loader = block.loaderName.isEmpty() ? tr("None") : QString(block.loaderName + ' ' + block.loaderVersion).trimmed();
        form->addRow(tr("Mod Loader"), plainLabel(loader, content));
        layout->addLayout(form);
    }
    layout->addStretch();
    m_pane->setWidget(content);
}

void PlaytimeDialog::run(QWidget* parent)
{
    PlaytimeDialog dialog(PlaytimeBreakdown::build(APPLICATION->instances()->playtimeEntries()), parent);
    dialog.exec();
}
