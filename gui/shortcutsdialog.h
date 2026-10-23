// Help ▸ Keyboard Shortcuts (Ctrl+/): every command with its shortcut and
// the menu it lives in, filterable, copyable as text. One window, raised on
// each request; the rows are read from the actions when it is shown.
#pragma once

#include <QDialog>
#include <QList>
#include <QPair>
#include <QString>

class QAction;
class QLineEdit;
class QTableWidget;

class ShortcutsDialog : public QDialog {
    Q_OBJECT
public:
    using Commands = QList<QPair<QString, QAction*>>;   // where, action
    static ShortcutsDialog* showFor(QWidget* parent, const Commands& commands);
    int     rowCount() const;
    QString rowText(int row) const;      // "Action\tShortcut\tWhere"

private:
    ShortcutsDialog(const Commands& commands, QWidget* parent);
    void fill();
    Commands      commands_;
    QLineEdit*    filter_ = nullptr;
    QTableWidget* table_  = nullptr;
};
