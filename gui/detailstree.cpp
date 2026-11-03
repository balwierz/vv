#include "detailstree.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QMenu>
#include <QShortcut>

#include <functional>

DetailsTree::DetailsTree(bool ownCopyShortcut, QWidget* parent) : QTreeWidget(parent) {
    setColumnCount(2);
    setHeaderLabels({tr("Item"), tr("Value")});
    setSelectionMode(QAbstractItemView::ExtendedSelection);
    setWordWrap(true);
    setUniformRowHeights(false);
    setContextMenuPolicy(Qt::CustomContextMenu);
    auto* copy = new QAction(tr("&Copy"), this);
    connect(copy, &QAction::triggered, this, [this] { copySelected(); });
    auto* copyAll = new QAction(tr("Copy &All"), this);
    connect(copyAll, &QAction::triggered, this, [this] {
        if (hasDetails_) QApplication::clipboard()->setText(allText());
    });
    connect(this, &QWidget::customContextMenuRequested, this, [this, copy, copyAll](const QPoint& p) {
        QMenu menu(this);
        menu.addAction(copy);
        menu.addAction(copyAll);
        copyAll->setEnabled(hasDetails_);
        menu.exec(viewport()->mapToGlobal(p));
    });
    if (ownCopyShortcut) {
        auto* sc = new QShortcut(QKeySequence::Copy, this);
        sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(sc, &QShortcut::activated, this, [this] { copySelected(); });
    }
}

void DetailsTree::setDetails(const FileDetails& d) {
    clear();
    details_ = d;
    hasDetails_ = true;
    std::function<void(QTreeWidgetItem*, const DetailItem&, int)> add =
        [&](QTreeWidgetItem* parent, const DetailItem& it, int depth) {
            const QString value = QString::fromStdString(it.value);
            // A value shows on one line (a writer's JSON runs to KB); the
            // tooltip and Copy have it whole.
            QString shown = value;
            shown.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
            if (shown.size() > 300) shown = shown.left(299) + QChar(0x2026);
            auto* item = parent ? new QTreeWidgetItem(parent, {QString::fromStdString(it.key), shown})
                                : new QTreeWidgetItem(this, {QString::fromStdString(it.key), shown});
            item->setData(1, Qt::UserRole, value);
            if (!value.isEmpty()) item->setToolTip(1, value.size() > 4000 ? value.left(4000) + QChar(0x2026) : value);
            if (depth == 0) {
                QFont f = item->font(0);
                f.setBold(true);
                item->setFont(0, f);
            }
            for (const DetailItem& c : it.children) add(item, c, depth + 1);
            // Sections open; a column's rows open on click, all of them
            // when there are few.
            const bool many = it.key == "Columns" && it.children.size() > 50;
            item->setExpanded(depth == 0 && !many);
        };
    for (const DetailItem& sec : d) add(nullptr, sec, 0);
    resizeColumnToContents(0);
}

void DetailsTree::showMessage(const QString& text) {
    clear();
    details_.clear();
    hasDetails_ = false;
    new QTreeWidgetItem(this, {text, {}});
}

bool DetailsTree::copySelected() {
    QStringList lines;
    for (QTreeWidgetItem* it : selectedItems())
        lines << it->text(0) + QLatin1Char('\t') + it->data(1, Qt::UserRole).toString();
    if (lines.isEmpty()) return false;
    QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')) + "\n");
    return true;
}

QString DetailsTree::allText() const {
    return hasDetails_ ? QString::fromStdString(details_text(details_)) : QString();
}
