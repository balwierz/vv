// Column widths at open: each column sized to its content (the 95th
// percentile of a sample of its cells, capped at 50 characters unless the tab
// shows its cells in full), the type line counted; bases / qualities columns
// in their fixed-width font.
#pragma once

#include <QFontMetrics>

#include <vector>

class ArrowTableModel;
class QTableView;

std::vector<int> planColumnWidths(ArrowTableModel* model, const QFontMetrics& fm,
                                  const QFontMetrics* typeFm = nullptr);
void autosizeColumns(QTableView* view, ArrowTableModel* model);
