#include "tablepane.h"

#include "arrowtablemodel.h"
#include "columnwidths.h"
#include "cursordelegate.h"
#include "loadsources.h"
#include "tabpane.h"
#include "tableview.h"
#include "typedheader.h"

#include <QHeaderView>
#include <QLabel>
#include <QSettings>
#include <QLineEdit>
#include <QVBoxLayout>

#include <limits>

VvTablePane::VvTablePane(bool filterBox, QWidget* parent)
    : QWidget(parent), pane_(new TabPane(this)), message_(new QLabel(this)) {
    const QSettings st(QStringLiteral("vv"), QStringLiteral("vvg"));
    passRows_ = st.value(QStringLiteral("limits/passRows"),
                         (qlonglong)ArrowTableModel::kDefaultPassRows).toLongLong();
    passBytes_ = st.value(QStringLiteral("limits/passBytes"),
                          (qlonglong)ArrowTableModel::kDefaultPassBytes).toLongLong();
    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(4);
    if (filterBox) {
        filter_ = new QLineEdit(this);
        filter_->setPlaceholderText(tr("Filter   e.g.  score > 5 and chrom == \"chr1\"   (Enter)"));
        filter_->setClearButtonEnabled(true);
        connect(filter_, &QLineEdit::returnPressed, this, [this] { applyFilter(); });
        lay->addWidget(filter_);
    }
    pane_->setDocumentMode(true);
    lay->addWidget(pane_, 1);
    message_->setWordWrap(true);
    message_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    message_->hide();
    lay->addWidget(message_);
    connect(pane_, &TabPane::currentChanged, this, [this](int i) { onCurrent(i); });
}

VvTablePane::~VvTablePane() { clear(); }

void VvTablePane::clear() {
    {
        QSignalBlocker b(pane_);
        while (pane_->count() > 0) pane_->removeTab(pane_->count() - 1);
    }
    for (Tab& t : tabs_) {
        // A source never shown goes on its file's thread, where its sibling
        // tabs (sharing its handles) are read.
        if (t.src && t.owner) {
            std::shared_ptr<TabularSource> dead = std::move(t.src);
            t.owner->post([dead]() mutable { dead.reset(); });
        }
        delete t.view;
        delete t.model;
        delete t.page;
    }
    tabs_.clear();
    message_->hide();
    if (filter_) filter_->clear();
}

QString VvTablePane::open(const QString& path) {
    clear();
    path_ = path;
    LoadResult r = loadSources(Config{}, {path}, /*expand=*/true);
    if (r.sources.empty()) {
        const QString why = r.errors.isEmpty() ? tr("%1: nothing to show").arg(path) : r.errors.join(QLatin1Char('\n'));
        message_->setText(why);
        message_->show();
        return why;
    }
    QSignalBlocker b(pane_);
    for (size_t i = 0; i < r.sources.size(); ++i) {
        Tab t;
        t.owner = r.owners[i];
        t.page = new QWidget;
        auto* lay = new QVBoxLayout(t.page);
        lay->setContentsMargins(0, 0, 0, 0);
        const QString label = QString::fromStdString(r.sources[i]->tab_label());   // reads no data
        t.src = std::move(r.sources[i]);
        tabs_.push_back(std::move(t));
        pane_->addTab(tabs_.back().page, label);
    }
    pane_->tabBar()->setVisible(tabs_.size() > 1);
    pane_->setCurrentIndex(0);
    onCurrent(0);
    return {};
}

int VvTablePane::tabCount() const { return (int)tabs_.size(); }
QString VvTablePane::tabLabel(int i) const { return pane_->label(i); }

ArrowTableModel* VvTablePane::modelAt(int i) {
    if (i < 0 || i >= (int)tabs_.size()) return nullptr;
    build(i);
    return tabs_[(size_t)i].model;
}
TableView* VvTablePane::viewAt(int i) {
    if (i < 0 || i >= (int)tabs_.size()) return nullptr;
    build(i);
    return tabs_[(size_t)i].view;
}

void VvTablePane::onCurrent(int i) {
    if (i < 0 || i >= (int)tabs_.size()) return;
    build(i);
    if (filter_) filter_->setText(tabs_[(size_t)i].filter);
}

// The tab's model and view, as vvg makes them: rows the font's height plus a
// margin (or the style's taller minimum), the type line under each name, the
// columns sized from the first rows read, a click on a header sorting.
void VvTablePane::build(int i) {
    Tab& t = tabs_[(size_t)i];
    if (t.model || !t.src) return;
    auto* model = new ArrowTableModel(std::move(t.src), this, t.owner, /*async=*/true);
    auto* view = new TableView(t.page);
    auto* header = new TypedHeaderView(view);
    header->setSectionsClickable(true);
    header->setHighlightSections(true);
    view->setHorizontalHeader(header);
    view->verticalHeader()->setDefaultSectionSize(view->fontMetrics().height() + 6);
    const int rowPx = std::max(view->verticalHeader()->defaultSectionSize(),
                               view->verticalHeader()->minimumSectionSize());
    model->setMaxViewRows(std::numeric_limits<int>::max() / rowPx - 1);
    // In a file manager's or Konqueror's process: a quarter of vvg's budget.
    model->setLoadBudget(ArrowTableModel::kDefaultLoadBudget / 4);
    view->setModel(model);
    view->setAlternatingRowColors(true);
    view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    view->setSelectionBehavior(QAbstractItemView::SelectItems);
    view->setItemDelegate(new CursorColumnDelegate(view));
    auto sized = std::make_shared<QMetaObject::Connection>();
    *sized = connect(model, &ArrowTableModel::chunksLoaded, view, [view, model, sized] {
        if (model->loadedPrefixRows() <= 0) return;
        QObject::disconnect(*sized);
        autosizeColumns(view, model);
    });
    for (QHeaderView* h : {view->horizontalHeader(), view->frozenView()->horizontalHeader()})
        connect(h, &QHeaderView::sectionClicked, this, [this, view, model](int section) {
            const auto& keys = model->sortKeys();
            const bool alone = keys.size() == 1 && keys.front().col == section;
            std::vector<ArrowTableModel::SortKey> next;
            if (!alone) next = {{section, Qt::AscendingOrder}};
            else if (keys.front().order == Qt::AscendingOrder) next = {{section, Qt::DescendingOrder}};
            if (!next.empty() && refuseFullPass(model, tr("Sorting"))) return;
            model->sortAsyncByKeys(next);
            view->horizontalHeader()->setSortIndicatorShown(!next.empty());
            if (!next.empty()) view->horizontalHeader()->setSortIndicator(section, next.front().order);
        });
    t.page->layout()->addWidget(view);
    t.view = view;
    t.model = model;
}

void VvTablePane::applyFilter() {
    const int i = pane_->currentIndex();
    if (!filter_ || i < 0 || i >= (int)tabs_.size() || !tabs_[(size_t)i].model) return;
    Tab& t = tabs_[(size_t)i];
    const QString text = filter_->text().trimmed();
    message_->hide();
    filter_->setStyleSheet({});
    if (text.isEmpty()) {
        t.filter.clear();
        t.model->clearFilterAsync();
        return;
    }
    FilterExpr fx;
    std::string err;
    if (!parse_filter_expr(text.toStdString(), *t.model->schema(), &fx, &err)) {
        filter_->setStyleSheet(QStringLiteral("background:#ffd6d6;"));
        message_->setText(tr("filter: %1").arg(QString::fromStdString(err)));
        message_->show();
        return;
    }
    if (refuseFullPass(t.model, tr("Filtering"))) return;
    t.filter = text;
    t.model->setFilterAsync(fx);
}

bool VvTablePane::refuseFullPass(ArrowTableModel* m, const QString& what) {
    if (!m->fullPassCost(path_, passRows_, passBytes_).over) return false;
    message_->setText(tr("%1 would read all of this large file into memory; open it in vvg to do that.").arg(what));
    message_->show();
    return true;
}
