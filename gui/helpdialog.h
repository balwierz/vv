// Help ▸ Field Reference (F1): how to use vvg, the syntax of its Filter,
// Find and Region boxes, the column types, and what the columns, keys and
// codes of AnnData, VCF, BED-family, SAM, Cooler and FASTQ files mean
// (gui/help/reference.html), with a section list and a search box. One
// window, raised and scrolled to an anchor on request.
#pragma once

#include <QDialog>
#include <QString>
#include <QStringList>

class QLabel;
class QLineEdit;
class QListWidget;
class QTextBrowser;

class HelpDialog : public QDialog {
    Q_OBJECT
public:
    // Shows the reference, at `anchor` when given.
    static HelpDialog* showReference(QWidget* parent, const QString& anchor = {});
    // The anchor that explains a column or key ("QUAL", "ClinVar.CLNSIG",
    // "AF_nfe", "AS_QD", "obsp"…), or "" when the reference has none.
    static QString anchorFor(const QString& key);
    // The section anchors, in order (tests).
    static QStringList sections();

private:
    explicit HelpDialog(QWidget* parent);
    void go(const QString& anchor);
    void find(bool next);
    QLineEdit*    search_   = nullptr;
    QListWidget*  sections_ = nullptr;
    QTextBrowser* text_     = nullptr;
    QLabel*       status_   = nullptr;
};
