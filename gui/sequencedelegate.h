// SequenceDelegate — draws a sequence column (SEQ of an alignment, a FASTA /
// FASTQ sequence, a VCF REF / ALT, the aligned sequences of a multiple
// alignment) in a fixed-width font with each base in its colour (A green,
// C blue, G orange, T / U red, N grey, as IGV), each amino acid in its
// class's colour (Clustal X's groups), or a quality string (SAM QUAL, FASTQ
// qual) with each character on a background from red (Phred 0) to green
// (Phred 40), the Phred score being the character's code minus 33. Gaps
// are grey. Only the characters that fit are drawn. Like the table's own
// delegate it tints the cursor's column.
#pragma once

#include <QStyledItemDelegate>

class QTableView;

class SequenceDelegate : public QStyledItemDelegate {
public:
    enum class Kind { Bases, Residues, Quality };
    SequenceDelegate(Kind kind, QTableView* view);
    Kind kind() const { return kind_; }
    // A multiple alignment's sequence column: wider by default, and a row
    // whose `plainColumn` cell (a number) is null — an annotation row:
    // #=GC SS_cons, Clustal's conservation line — is drawn uncoloured.
    void setAlignment(int plainColumn) { alignment_ = true; plainColumn_ = plainColumn; }
    bool isAlignment() const { return alignment_; }
    // The width of one character and the inset of the first from the cell's
    // left edge, as paint() draws them in `view` (the header's ruler).
    static int charAdvance(const QWidget* view);
    static int inset(const QWidget* view);
    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override;

    // A base's colour (case ignored); invalid for a character that is not a
    // base. `dark`: for a dark palette.
    static QColor baseColor(QChar c, bool dark);
    // An amino acid's colour by its class (Clustal X): hydrophobic blue,
    // positive red, negative magenta, polar green, G orange, P yellow, C pink,
    // aromatic H / Y cyan; invalid for anything else.
    static QColor residueColor(QChar c, bool dark);
    // The background of a quality character of Phred score `q`.
    static QColor qualityColor(int q);

private:
    Kind        kind_;
    QTableView* view_;
    bool        alignment_ = false;
    int         plainColumn_ = -1;
};
