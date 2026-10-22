#include "tabpane.h"

TabPane::TabPane(QWidget* parent)
    : QWidget(parent),
      layout_(new QVBoxLayout(this)),
      bar_(new QTabBar(this)),
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
    stack_->setCurrentIndex(bar_->currentIndex());
    return at;
}

void TabPane::removeTab(int index) {
    if (index < 0 || index >= count()) return;
    QWidget* page = stack_->widget(index);
    stack_->removeWidget(page);
    bar_->removeTab(index);
    if (bar_->currentIndex() >= 0) stack_->setCurrentIndex(bar_->currentIndex());
}
