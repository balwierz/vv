// SequenceDelegate — draws a sequence column (SEQ of an alignment, a FASTA /
// FASTQ sequence, a VCF REF / ALT) in a fixed-width font with each base in
// its colour (A green, C blue, G orange, T / U red, N grey, as IGV), or a
// quality string (SAM QUAL, FASTQ qual) with each character on a background
// from red (Phred 0) to green (Phred 40), the Phred score being the
// character's code minus 33. Only the characters that fit are drawn. Like
// the table's own delegate it tints the cursor's column.
#pragma once

#include <QStyledItemDelegate>

class QTableView;

class SequenceDelegate : public QStyledItemDelegate {
public:
    enum class Kind { Bases, Quality };
    SequenceDelegate(Kind kind, QTableView* view);
    Kind kind() const { return kind_; }
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;

    // A base's colour (case ignored); invalid for a character that is not a
    // base. `dark`: for a dark palette.
    static QColor baseColor(QChar c, bool dark);
    // The background of a quality character of Phred score `q`.
    static QColor qualityColor(int q);

private:
    Kind        kind_;
    QTableView* view_;
};
