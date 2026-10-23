// TabPane — the window's tabs: a tab bar, a strip under it (the context bar
// of the current tab's format options) and the pages. QTabWidget puts its
// pages directly under its bar, with no room between; this keeps the part of
// QTabWidget's interface that vvg uses, so the tab code reads the same.
//
// Tabs can be grouped (vvg: by the file they come from): with two or more
// groups, each run of tabs of one group is underlined in its colour.
// tabText() is what the bar shows; label() the text the tab was added with.
#pragma once

#include <functional>

#include <QStackedWidget>
#include <QTabBar>
#include <QVBoxLayout>
#include <QWidget>

class TabPane : public QWidget {
    Q_OBJECT
public:
    explicit TabPane(QWidget* parent = nullptr);

    // `strip` shows between the bar and the pages (owned by the pane).
    void setStrip(QWidget* strip);

    int      addTab(QWidget* page, const QString& label) { return insertTab(count(), page, label); }
    int      insertTab(int index, QWidget* page, const QString& label);
    void     removeTab(int index);          // the page is not deleted
    int      count() const { return bar_->count(); }
    int      currentIndex() const { return bar_->currentIndex(); }
    QWidget* currentWidget() const { return stack_->currentWidget(); }
    QWidget* widget(int index) const { return stack_->widget(index); }
    int      indexOf(QWidget* page) const { return stack_->indexOf(page); }
    void     setCurrentIndex(int index) { bar_->setCurrentIndex(index); }
    void     setCurrentWidget(QWidget* page) { setCurrentIndex(indexOf(page)); }
    QString  tabText(int index) const { return bar_->tabText(index); }
    QString  label(int index) const { return bar_->tabData(index).toString(); }
    void     setTabText(int index, const QString& t) { bar_->setTabText(index, t); }
    void     setTabToolTip(int index, const QString& t) { bar_->setTabToolTip(index, t); }
    void     setTabsClosable(bool on) { bar_->setTabsClosable(on); }
    void     setDocumentMode(bool on) { bar_->setDocumentMode(on); }
    QTabBar* tabBar() const { return bar_; }
    // `groupOf(i)`: tab i's group, 0 for none; `colorOf(g)`: group g's colour.
    void setGroups(std::function<int(int)> groupOf, std::function<QColor(int)> colorOf);

signals:
    void currentChanged(int index);
    void tabCloseRequested(int index);

private:
    QVBoxLayout*    layout_;
    QTabBar*        bar_;
    QWidget*        strip_ = nullptr;
    QStackedWidget* stack_;
};
