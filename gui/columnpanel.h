// The Column tab of vvg's right dock: one column's summary (count, nulls, sum,
// mean, std, min / percentiles / max, distinct) and its most frequent values,
// as copyable tables. It only displays; MainWindow computes the summaries
// (summarize_columns on a worker) and tells it what to show.
#pragma once
#include <QWidget>

#include "vv/vvcore.hpp"

class QLabel;
class QVBoxLayout;
class QProgressBar;
class QPushButton;
class QTableWidget;

class ColumnPanel : public QWidget {
    Q_OBJECT
public:
    explicit ColumnPanel(QWidget* parent = nullptr);

    // A finished summary. `digits`: significant digits for its numbers (see
    // ArrowTableModel::summableDigits; 15 when 0); `type` formats a date /
    // timestamp column's statistics as dates.
    void showSummary(const ColumnSummary& s, int digits,
                     const std::shared_ptr<arrow::DataType>& type, const QString& scope);
    // A scan in progress: rows read so far, of `total` (< 0: unknown).
    void showBusy(const QString& title, const QString& text, qint64 rows, qint64 total);
    // Nothing to summarise, or an error; `offerCompute` shows the Compute button.
    void showNote(const QString& title, const QString& text, bool offerCompute = false);
    // Where the column is in the table: display column `col` (0-based) of
    // `count`, and whether it is out of view (scrolled away or hidden).
    // col < 0: no column. While set, the column name in the title is a link
    // that asks for the column to be shown (showColumnRequested).
    void setLocation(int col, int count, bool outOfView, bool hidden = false);

    const ColumnSummary* summary() const { return has_ ? &sum_ : nullptr; }
    // The selected cells of the focused table as TSV (all of it when nothing
    // is selected); false when the panel shows no summary.
    bool copySelection();
    QString allAsTsv() const;
    // "statistic=value …" for the window self-test.
    QString describeForTest() const;
    // The title and location lines as plain text, for the window self-test.
    QString titleForTest() const { return titlePlain_; }
    QString locationForTest() const;

signals:
    void cancelRequested();
    void computeRequested();
    void filterToValueRequested(int valueIndex);   // index into summary()->values
    void copyCommandRequested();
    void showColumnRequested();

private:
    void showTables(bool on);
    void setTitle(const QString& plain, const QString& suffix = {});
    void renderLocation();
    void contextMenu(QTableWidget* t, const QPoint& pos);

    QVBoxLayout*  lay_     = nullptr;
    QLabel*       title_   = nullptr;
    QLabel*       scope_   = nullptr;
    QLabel*       where_   = nullptr;
    QTableWidget* stats_   = nullptr;
    QLabel*       topHead_ = nullptr;
    QTableWidget* top_     = nullptr;
    QWidget*      busy_    = nullptr;
    QProgressBar* bar_     = nullptr;
    QLabel*       note_    = nullptr;
    QPushButton*  compute_ = nullptr;
    QString       titlePlain_;     // the title without markup ("name · type")
    QString       titleName_;      // its column-name part (the link)
    QString       titleSuffix_;
    int           locCol_ = -1, locCount_ = 0;
    bool          locOut_ = false, locHidden_ = false;
    ColumnSummary sum_;
    bool          has_ = false;
};
