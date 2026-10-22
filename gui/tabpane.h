// TabPane — the window's tabs: a tab bar, a strip under it (the context bar
// of the current tab's format options) and the pages. QTabWidget puts its
// pages directly under its bar, with no room between; this keeps the part of
// QTabWidget's interface that vvg uses, so the tab code reads the same.
#pragma once

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
    void     setTabText(int index, const QString& t) { bar_->setTabText(index, t); }
    void     setTabToolTip(int index, const QString& t) { bar_->setTabToolTip(index, t); }
    void     setTabsClosable(bool on) { bar_->setTabsClosable(on); }
    void     setDocumentMode(bool on) { bar_->setDocumentMode(on); }
    QTabBar* tabBar() const { return bar_; }

signals:
    void currentChanged(int index);
    void tabCloseRequested(int index);

private:
    QVBoxLayout*    layout_;
    QTabBar*        bar_;
    QWidget*        strip_ = nullptr;
    QStackedWidget* stack_;
};
