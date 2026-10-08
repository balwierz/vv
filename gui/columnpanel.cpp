#include "columnpanel.h"

#include <QApplication>
#include <QClipboard>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <cmath>

namespace {

// Plain numbers (no digit grouping), so a copied value pastes as a number.
QString num(double v, int digits) {
    if (std::isnan(v)) return QStringLiteral("nan");
    if (std::isinf(v)) return v > 0 ? QStringLiteral("inf") : QStringLiteral("-inf");
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.*g", digits, v);
    return QString::fromLatin1(buf);
}

QTableWidget* makeTable(QWidget* parent, const QStringList& head) {
    auto* t = new QTableWidget(0, head.size(), parent);
    t->setHorizontalHeaderLabels(head);
    t->verticalHeader()->setVisible(false);
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionMode(QAbstractItemView::ExtendedSelection);
    t->setContextMenuPolicy(Qt::CustomContextMenu);
    t->setWordWrap(false);
    t->horizontalHeader()->setStretchLastSection(true);
    // Compact rows: the default section size leaves half of each row empty.
    t->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed);
    t->verticalHeader()->setDefaultSectionSize(t->fontMetrics().height() + 6);
    return t;
}

QTableWidgetItem* item(const QString& text, bool right = false) {
    auto* it = new QTableWidgetItem(text);
    if (right) it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return it;
}

// The table as TSV: the selected cells, or every cell when `all`.
QString tableTsv(const QTableWidget* t, bool all) {
    QString out;
    for (int r = 0; r < t->rowCount(); ++r) {
        QStringList cells;
        for (int c = 0; c < t->columnCount(); ++c) {
            const QTableWidgetItem* it = t->item(r, c);
            if (it && (all || it->isSelected())) cells << it->text();
        }
        if (!cells.isEmpty()) out += cells.join(QLatin1Char('\t')) + QLatin1Char('\n');
    }
    return out;
}

}  // namespace

ColumnPanel::ColumnPanel(QWidget* parent) : QWidget(parent) {
    auto* lay = new QVBoxLayout(this);
    title_ = new QLabel(this);
    title_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont bold = title_->font();
    bold.setBold(true);
    title_->setFont(bold);
    title_->setWordWrap(true);
    scope_ = new QLabel(this);
    scope_->setWordWrap(true);
    scope_->setTextInteractionFlags(Qt::TextSelectableByMouse);

    stats_ = makeTable(this, {tr("Statistic"), tr("Value")});
    topHead_ = new QLabel(this);
    topHead_->setWordWrap(true);
    top_ = makeTable(this, {tr("Value"), tr("Count"), tr("%")});
    // The value takes the width; count and share fit their numbers.
    top_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    top_->horizontalHeader()->setStretchLastSection(false);
    top_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    top_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    top_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    stats_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    stats_->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    top_->setToolTip(tr("Double-click a value to filter the table to it"));
    connect(top_, &QTableWidget::cellDoubleClicked, this,
            [this](int row, int) { if (has_) emit filterToValueRequested(row); });
    connect(stats_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& p) { contextMenu(stats_, p); });
    connect(top_, &QWidget::customContextMenuRequested, this,
            [this](const QPoint& p) { contextMenu(top_, p); });

    busy_ = new QWidget(this);
    auto* bl = new QHBoxLayout(busy_);
    bl->setContentsMargins(0, 0, 0, 0);
    bar_ = new QProgressBar(busy_);
    auto* cancel = new QPushButton(tr("Cancel"), busy_);
    connect(cancel, &QPushButton::clicked, this, &ColumnPanel::cancelRequested);
    bl->addWidget(bar_, 1);
    bl->addWidget(cancel);

    note_ = new QLabel(this);
    note_->setWordWrap(true);
    note_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    compute_ = new QPushButton(tr("Compute"), this);
    connect(compute_, &QPushButton::clicked, this, &ColumnPanel::computeRequested);

    lay->addWidget(title_);
    lay->addWidget(scope_);
    lay->addWidget(stats_);
    lay->addWidget(topHead_);
    lay->addWidget(top_, 1);
    lay->addWidget(busy_);
    lay->addWidget(note_);
    lay->addWidget(compute_, 0, Qt::AlignLeft);
    lay->addStretch(1);
    lay_ = lay;
    showNote(tr("Column"), tr("Put the cursor in a table column to summarise it."));
}

void ColumnPanel::showTables(bool on) {
    stats_->setVisible(on);
    topHead_->setVisible(on);
    const bool top = on && top_->rowCount() > 0;
    top_->setVisible(top);
    // The values list fills the panel; without it the trailing stretch keeps
    // the rest at the top.
    lay_->setStretch(lay_->count() - 1, top ? 0 : 1);
}

void ColumnPanel::showSummary(const ColumnSummary& s, int digits,
                              const std::shared_ptr<arrow::DataType>& type,
                              const QString& scope) {
    sum_ = s;
    has_ = true;
    if (digits <= 0) digits = 15;
    // A mean / std / percentile: 10 significant digits are plenty to read.
    const int fd = std::min(digits, 10);
    title_->setText(QStringLiteral("%1 · %2").arg(QString::fromStdString(s.name),
                                                  QString::fromStdString(s.type)));
    scope_->setText(scope);
    const int64_t rows = s.count + s.nulls;
    auto stat = [&](double v, int dg) {
        return s.temporal ? QString::fromStdString(format_temporal_value(v, type)) : num(v, dg);
    };
    QList<QPair<QString, QString>> lines;
    QStringList tips;   // parallel to lines
    auto add = [&](const QString& k, const QString& v, const QString& tip = {}) {
        lines.append({k, v});
        tips << tip;
    };
    add(tr("Count"), QString::number(s.count), tr("values that are not null"));
    add(tr("Nulls"), QString::number(s.nulls),
        rows ? tr("%1% of %2 rows").arg(100.0 * (double)s.nulls / (double)rows, 0, 'f', 1).arg(rows)
             : QString());
    const bool has_values = s.count > 0 && std::isfinite(s.min) && std::isfinite(s.max);
    if (s.numeric && has_values) {
        if (!s.temporal) add(tr("Sum"), num(s.sum, digits));
        add(tr("Mean"), stat(s.mean, fd));
        if (!s.temporal) add(tr("Std"), s.has_std ? num(s.std, fd) : QStringLiteral("-"),
                              tr("standard deviation, n - 1 denominator"));
        add(tr("Min"), stat(s.min, digits));
        static const char* kPct[] = {"25%", "Median", "75%"};
        const QString approx = s.percentiles_sampled
            ? tr("estimated from a uniform random sample of the values") : QString();
        for (size_t i = 0; i < s.percentiles.size() && i < 3; ++i)
            add(QString::fromLatin1(kPct[i]),
                (s.percentiles_sampled ? QStringLiteral("~") : QString()) +
                    stat(s.percentiles[i], fd),
                approx);
        add(tr("Max"), stat(s.max, digits));
    } else if (!s.numeric && s.count > 0) {
        add(tr("Min"), QString::fromStdString(s.s_min));
        add(tr("Max"), QString::fromStdString(s.s_max));
    }
    add(tr("Distinct"), s.distinct >= 0 ? QString::number(s.distinct) : tr("more than 10000"),
        tr("distinct values that are not null"));
    stats_->setRowCount(lines.size());
    for (int r = 0; r < lines.size(); ++r) {
        auto* k = item(lines[r].first);
        auto* v = item(lines[r].second, true);
        if (!tips[r].isEmpty()) { k->setToolTip(tips[r]); v->setToolTip(tips[r]); }
        stats_->setItem(r, 0, k);
        stats_->setItem(r, 1, v);
    }
    stats_->resizeColumnToContents(0);
    // Every statistic in view without scrolling; the values list takes the rest.
    stats_->setFixedHeight(stats_->horizontalHeader()->height() +
                           stats_->verticalHeader()->defaultSectionSize() * stats_->rowCount() +
                           2 * stats_->frameWidth());

    // The 50 most frequent values.
    const int n = (int)std::min<size_t>(s.values.size(), 50);
    top_->setRowCount(n);
    for (int r = 0; r < n; ++r) {
        const ValueCount& vc = s.values[r];
        auto* v = item(vc.null ? tr("(null)") : QString::fromStdString(vc.value));
        if (vc.null) { QFont f = v->font(); f.setItalic(true); v->setFont(f); }
        top_->setItem(r, 0, v);
        top_->setItem(r, 1, item(QString::number(vc.count), true));
        top_->setItem(r, 2, item(rows ? QString::number(100.0 * (double)vc.count / (double)rows, 'f', 1)
                                      : QString(), true));
    }
    if (s.values.empty() && s.distinct < 0)
        topHead_->setText(tr("Top values: more than 10000 distinct values; not counted."));
    else if ((int)s.values.size() > n)
        topHead_->setText(tr("Top values (%1 of %2):").arg(n).arg(s.values.size()));
    else
        topHead_->setText(tr("Values:"));
    busy_->setVisible(false);
    note_->setVisible(false);
    compute_->setVisible(false);
    showTables(true);
}

void ColumnPanel::showBusy(const QString& title, const QString& text, qint64 rows, qint64 total) {
    has_ = false;
    title_->setText(title);
    scope_->setText(text.arg(QLocale().toString(rows)));
    if (total > 0) { bar_->setRange(0, 1000); bar_->setValue((int)std::min<qint64>(1000, rows * 1000 / total)); }
    else bar_->setRange(0, 0);
    showTables(false);
    busy_->setVisible(true);
    note_->setVisible(false);
    compute_->setVisible(false);
}

void ColumnPanel::showNote(const QString& title, const QString& text, bool offerCompute) {
    has_ = false;
    title_->setText(title);
    scope_->clear();
    note_->setText(text);
    showTables(false);
    busy_->setVisible(false);
    note_->setVisible(!text.isEmpty());
    compute_->setVisible(offerCompute);
}

bool ColumnPanel::copySelection() {
    if (!has_) return false;
    QTableWidget* t = top_->hasFocus() ? top_ : stats_;
    const bool any = !t->selectedItems().isEmpty();
    QApplication::clipboard()->setText(tableTsv(t, !any));
    return true;
}

QString ColumnPanel::allAsTsv() const {
    QString out = title_->text() + QLatin1Char('\n') + tableTsv(stats_, true);
    if (top_->rowCount() > 0)
        out += QLatin1Char('\n') + tr("Value\tCount\t%") + QLatin1Char('\n') + tableTsv(top_, true);
    return out;
}

QString ColumnPanel::describeForTest() const {
    if (!has_) return note_->isVisible() ? QStringLiteral("note: ") + note_->text()
                                         : QStringLiteral("busy");
    QStringList parts;
    for (int r = 0; r < stats_->rowCount(); ++r)
        parts << stats_->item(r, 0)->text().toLower() + QLatin1Char('=') + stats_->item(r, 1)->text();
    QStringList top;
    for (int r = 0; r < std::min(3, top_->rowCount()); ++r)
        top << top_->item(r, 0)->text() + QLatin1Char(':') + top_->item(r, 1)->text();
    return parts.join(QLatin1Char(' ')) + QStringLiteral(" top=") + top.join(QLatin1Char(',')) +
           QStringLiteral(" scope=") + scope_->text();
}

void ColumnPanel::contextMenu(QTableWidget* t, const QPoint& pos) {
    if (!has_) return;
    QMenu menu(this);
    QAction* cp = menu.addAction(tr("&Copy"));
    QAction* all = menu.addAction(tr("Copy &All as TSV"));
    QAction* cmd = menu.addAction(tr("Copy as &vv Command"));
    QAction* chosen = menu.exec(t->viewport()->mapToGlobal(pos));
    if (chosen == cp) {
        const bool any = !t->selectedItems().isEmpty();
        QApplication::clipboard()->setText(tableTsv(t, !any));
    } else if (chosen == all) {
        QApplication::clipboard()->setText(allAsTsv());
    } else if (chosen == cmd) {
        emit copyCommandRequested();
    }
}
