// Help ▸ Find Action (Ctrl+Shift+P): a popup over every command, filtered as
// you type (name, shortcut, the menu it is in; every word must match); Enter
// runs the selected one. The same widget as Genome Goggles' palette.
#pragma once

#include <QFrame>
#include <QList>
#include <QPair>
#include <QString>

class QAction;
class QLineEdit;
class QListView;
class QSortFilterProxyModel;
class QStandardItemModel;

class ActionPalette : public QFrame {
    Q_OBJECT
public:
    explicit ActionPalette(const QList<QPair<QString, QAction*>>& commands, QWidget* parent = nullptr);
    // Shows the popup with its top centre at `global_top_centre`.
    void popupAt(const QPoint& global_top_centre);
    void setQuery(const QString& text);
    QStringList shown() const;   // the names listed (tests)
    void runSelected();

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    QLineEdit* query_ = nullptr;
    QListView* list_ = nullptr;
    QStandardItemModel* model_ = nullptr;
    QSortFilterProxyModel* proxy_ = nullptr;
};
