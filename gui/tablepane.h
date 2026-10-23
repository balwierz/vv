// VvTablePane — one file as tables, without vvg's window around it: its tabs
// (sheets, tables, AnnData parts, …) in a TabPane, each a TableView over an
// ArrowTableModel read on the file's own thread, built when first shown,
// sized to its content, sorted by a header click (ascending, descending,
// off). With `filterBox`, a box above them takes vv's --filter expressions
// for the shown tab. The table widget of the KDE part (vvpart) and of the
// Table page of Dolphin's Properties dialog (vvproperties).
#pragma once

#include <QString>
#include <QWidget>

#include <memory>
#include <vector>

class ArrowTableModel;
class QLabel;
class QLineEdit;
class SourceOwner;
class TableView;
class TabPane;
class TabularSource;

class VvTablePane : public QWidget {
    Q_OBJECT
public:
    explicit VvTablePane(bool filterBox, QWidget* parent = nullptr);
    ~VvTablePane() override;

    // Open `path` (through libvvcore, packed INFO / attributes columns
    // expanded): "" or why it failed, which the pane then shows.
    QString open(const QString& path);
    void    clear();
    int     tabCount() const;
    QString tabLabel(int i) const;
    // Tab `i`'s model, built if it was not shown yet.
    ArrowTableModel* modelAt(int i);
    TableView*       viewAt(int i);

private:
    struct Tab {
        std::unique_ptr<TabularSource> src;      // until the tab is built
        std::shared_ptr<SourceOwner>   owner;
        QWidget*                       page = nullptr;
        TableView*                     view = nullptr;
        ArrowTableModel*               model = nullptr;
        QString                        filter;   // as applied
    };
    void build(int i);
    void applyFilter();
    void onCurrent(int i);

    std::vector<Tab> tabs_;
    TabPane*         pane_;
    QLineEdit*       filter_ = nullptr;
    QLabel*          message_;
};
