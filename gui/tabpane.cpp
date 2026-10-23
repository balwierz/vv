#include "tabpane.h"

#include <QPainter>
#include <QSet>

namespace {
// The tab bar, with a line in the group's colour under each run of tabs of
// one group (one file), drawn when there are two groups or more.
class GroupedTabBar : public QTabBar {
public:
    using QTabBar::QTabBar;
    std::function<int(int)>    groupOf;
    std::function<QColor(int)> colorOf;

protected:
    void paintEvent(QPaintEvent* e) override {
        QTabBar::paintEvent(e);
        if (!groupOf || !colorOf || count() < 2) return;
        QSet<int> groups;
        for (int i = 0; i < count(); ++i)
            if (int g = groupOf(i)) groups.insert(g);
        if (groups.size() < 2) return;
        QPainter p(this);
        for (int i = 0; i < count();) {
            const int g = groupOf(i);
            int j = i;
            while (j + 1 < count() && groupOf(j + 1) == g) ++j;
            if (g) {
                const QRect r = tabRect(i).united(tabRect(j));
                p.fillRect(QRect(r.left() + 2, r.bottom() - 2, r.width() - 4, 3), colorOf(g));
            }
            i = j + 1;
        }
    }
};
}   // namespace

TabPane::TabPane(QWidget* parent)
    : QWidget(parent),
      layout_(new QVBoxLayout(this)),
      bar_(new GroupedTabBar(this)),
      stack_(new QStackedWidget(this)) {
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(0);
    bar_->setExpanding(false);
    bar_->setElideMode(Qt::ElideRight);
    bar_->setUsesScrollButtons(true);
    layout_->addWidget(bar_);
    layout_->addWidget(stack_, 1);
    // The page follows the bar; the bar's signal is the pane's (blocking the
    // pane's signals leaves the page in step).
    connect(bar_, &QTabBar::currentChanged, this, [this](int i) {
        if (i >= 0 && i < stack_->count()) stack_->setCurrentIndex(i);
        emit currentChanged(i);
    });
    connect(bar_, &QTabBar::tabCloseRequested, this, &TabPane::tabCloseRequested);
}

void TabPane::setStrip(QWidget* strip) {
    if (strip_) layout_->removeWidget(strip_);
    strip_ = strip;
    if (strip_) {
        strip_->setParent(this);
        layout_->insertWidget(1, strip_);
    }
}

int TabPane::insertTab(int index, QWidget* page, const QString& label) {
    if (index < 0 || index > count()) index = count();
    // The page first: the bar's currentChanged (the first tab added becomes
    // current) then finds it at the same index.
    stack_->insertWidget(index, page);
    const int at = bar_->insertTab(index, label);
    bar_->setTabData(at, label);
    stack_->setCurrentIndex(bar_->currentIndex());
    return at;
}

void TabPane::setGroups(std::function<int(int)> groupOf, std::function<QColor(int)> colorOf) {
    auto* bar = static_cast<GroupedTabBar*>(bar_);
    bar->groupOf = std::move(groupOf);
    bar->colorOf = std::move(colorOf);
    bar->update();
}

void TabPane::removeTab(int index) {
    if (index < 0 || index >= count()) return;
    QWidget* page = stack_->widget(index);
    stack_->removeWidget(page);
    bar_->removeTab(index);
    if (bar_->currentIndex() >= 0) stack_->setCurrentIndex(bar_->currentIndex());
}
