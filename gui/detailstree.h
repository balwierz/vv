// DetailsTree — file_details() as a two-column tree (Item | Value): sections
// bold and expanded, a Columns section of more than 50 columns collapsed,
// each value on one line with the whole of it in the tooltip and in Copy.
// The context menu copies the selected rows as "key<TAB>value" or all of it
// as the indented text `vv --info` prints. vvg's Details ▸ File tab and the
// Format page of Dolphin's Properties dialog (vvproperties).
#pragma once

#include <QString>
#include <QTreeWidget>

#include "vv/vvcore.hpp"

class DetailsTree : public QTreeWidget {
    Q_OBJECT
public:
    // `ownCopyShortcut`: Ctrl+C on the tree copies the selected rows. vvg
    // passes false: its window's Copy routes here by focus.
    explicit DetailsTree(bool ownCopyShortcut, QWidget* parent = nullptr);

    void setDetails(const FileDetails& d);
    // One line in place of the tree: "Reading…", "No file", an error.
    void showMessage(const QString& text);
    bool hasDetails() const { return hasDetails_; }

    // The selected rows, values whole, to the clipboard; false when none.
    bool    copySelected();
    QString allText() const;      // as `vv --info` prints it; "" before setDetails

private:
    FileDetails details_;
    bool        hasDetails_ = false;
};
