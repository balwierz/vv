#include "contextbar.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QPair>
#include <QLineEdit>
#include <QSpinBox>
#include <QToolButton>

namespace {
// Left to right, wrapping to a new line where the bar is too narrow, each
// line's items centred on it (Qt's flow layout example, trimmed). A narrow
// window then shows every control on more lines instead of growing wider.
class FlowLayout : public QLayout {
public:
    FlowLayout(QWidget* parent, int hSpacing, int vSpacing)
        : QLayout(parent), h_(hSpacing), v_(vSpacing) {}
    ~FlowLayout() override { while (QLayoutItem* i = takeAt(0)) delete i; }
    void addItem(QLayoutItem* item) override { items_.append(item); }
    int count() const override { return (int)items_.size(); }
    QLayoutItem* itemAt(int i) const override { return items_.value(i); }
    QLayoutItem* takeAt(int i) override {
        return i >= 0 && i < items_.size() ? items_.takeAt(i) : nullptr;
    }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return place(QRect(0, 0, w, 0), false); }
    void setGeometry(const QRect& r) override { QLayout::setGeometry(r); place(r, true); }
    QSize sizeHint() const override {          // everything on one line
        int w = 0, h = 0;
        for (QLayoutItem* i : items_) {
            if (i->isEmpty()) continue;
            w += i->sizeHint().width() + h_;
            h = std::max(h, i->sizeHint().height());
        }
        const QMargins m = contentsMargins();
        return {w + m.left() + m.right(), h + m.top() + m.bottom()};
    }
    QSize minimumSize() const override {       // the widest single item
        QSize s;
        for (QLayoutItem* i : items_) if (!i->isEmpty()) s = s.expandedTo(i->minimumSize());
        const QMargins m = contentsMargins();
        return s + QSize(m.left() + m.right(), m.top() + m.bottom());
    }

private:
    // Lay the items out in `rect` (when `apply`); returns the height used.
    int place(const QRect& rect, bool apply) const {
        const QRect r = rect.marginsRemoved(contentsMargins());
        int y = r.y();
        QList<QPair<QLayoutItem*, QSize>> line;
        int lineW = 0, lineH = 0;
        auto flush = [&] {
            int x = r.x();
            for (const auto& [item, sz] : line) {
                if (apply) item->setGeometry(QRect(QPoint(x, y + (lineH - sz.height()) / 2), sz));
                x += sz.width() + h_;
            }
            if (!line.isEmpty()) y += lineH + v_;
            line.clear();
            lineW = lineH = 0;
        };
        for (QLayoutItem* i : items_) {
            if (i->isEmpty()) continue;
            QSize sz = i->sizeHint();
            sz.setWidth(std::min(sz.width(), std::max(1, r.width())));
            if (!line.isEmpty() && lineW + h_ + sz.width() > r.width()) flush();
            lineW += (line.isEmpty() ? 0 : h_) + sz.width();
            lineH = std::max(lineH, sz.height());
            line.append({i, sz});
        }
        flush();
        const int used = std::max(0, y - v_ - r.y());
        return used + contentsMargins().top() + contentsMargins().bottom();
    }
    QList<QLayoutItem*> items_;
    int h_, v_;
};

// The region box: 240 px wide where the line has room, down to 140 px.
class RegionEdit : public QLineEdit {
public:
    using QLineEdit::QLineEdit;
    QSize sizeHint() const override { QSize s = QLineEdit::sizeHint(); s.setWidth(240); return s; }
};

const char* kSampleLayouts[] = {"struct", "long", "text"};
const char* kMatrixLayouts[] = {"wide", "long"};

// Segmented choice: one checkable button per value, one checked at a time.
QButtonGroup* segmented(QWidget* parent, QHBoxLayout* lay, std::initializer_list<const char*> values,
                        std::initializer_list<const char*> tips) {
    auto* g = new QButtonGroup(parent);
    g->setExclusive(true);
    int id = 0;
    auto tip = tips.begin();
    for (const char* v : values) {
        auto* b = new QToolButton(parent);
        b->setText(QString::fromLatin1(v));
        b->setCheckable(true);
        b->setAutoRaise(true);
        if (tip != tips.end()) b->setToolTip(QString::fromUtf8(*tip++));
        g->addButton(b, id++);
        lay->addWidget(b);
    }
    return g;
}
}  // namespace

QWidget* ContextBar::group(QHBoxLayout** lay) {
    auto* w = new QWidget(this);
    *lay = new QHBoxLayout(w);
    (*lay)->setContentsMargins(0, 0, 0, 0);
    (*lay)->setSpacing(4);
    return w;
}

QToolButton* ContextBar::toggle(const QString& text, const QString& tip) {
    auto* b = new QToolButton(this);
    b->setText(text);
    b->setToolTip(tip);
    b->setCheckable(true);
    b->setAutoRaise(true);
    connect(b, &QToolButton::toggled, this, [this] { apply(); });
    return b;
}

ContextBar::ContextBar(QWidget* parent) : QWidget(parent) {
    auto* lay = new FlowLayout(this, 14, 2);
    lay->setContentsMargins(6, 3, 6, 3);

    // Region: the box, Go, the coordinate convention and the slop.
    QHBoxLayout* rl = nullptr;
    regionGroup_ = group(&rl);
    rl->addWidget(new QLabel(tr("Region"), regionGroup_));
    regionEdit_ = new RegionEdit(regionGroup_);
    regionEdit_->setPlaceholderText(tr("chr1:1,000-2,000[,chr2:…]"));
    regionEdit_->setClearButtonEnabled(true);
    regionEdit_->setMinimumWidth(140);
    regionEdit_->setToolTip(tr("Show only the records in these windows (Enter applies; Ctrl+L)"));
    rl->addWidget(regionEdit_, 1);
    goButton_ = new QToolButton(regionGroup_);
    goButton_->setText(tr("Go"));
    goButton_->setAutoRaise(true);
    goButton_->setToolTip(tr("Apply the region (Enter)"));
    rl->addWidget(goButton_);
    coords_ = new QComboBox(regionGroup_);
    coords_->addItem(tr("0-based"));   // index 0: UCSC
    coords_->addItem(tr("1-based"));   // index 1: NCBI
    coords_->setToolTip(tr("The region's coordinates: UCSC 0-based half-open, or NCBI "
                           "1-based inclusive"));
    rl->addWidget(coords_);
    rl->addWidget(new QLabel(tr("± slop"), regionGroup_));
    slop_ = new QSpinBox(regionGroup_);
    slop_->setRange(0, 1000000000);
    slop_->setSingleStep(100);
    slop_->setSuffix(tr(" bp"));
    slop_->setToolTip(tr("Pad each window by this many bases on both sides"));
    slop_->setMaximumWidth(110);
    rl->addWidget(slop_);
    lay->addWidget(regionGroup_);
    connect(regionEdit_, &QLineEdit::returnPressed, this, [this] { apply(); });
    connect(goButton_, &QToolButton::clicked, this, [this] { apply(); });
    connect(regionEdit_, &QLineEdit::textChanged, this, [this](const QString& t) {
        if (t.isEmpty() && !opts_.region.empty()) apply();   // the clear button
    });
    connect(coords_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!opts_.region.empty()) apply();
    });
    connect(slop_, &QSpinBox::editingFinished, this, [this] {
        if (!opts_.region.empty() && slop_->value() != opts_.slop) apply();
    });

    // Alignments: per-base pileup rows, aux tag columns.
    QHBoxLayout* al = nullptr;
    alignGroup_ = group(&al);
    pileup_ = toggle(tr("Pileup"), tr("BAM/CRAM: one row per reference position with the bases "
                                      "and qualities piled on it (samtools mpileup)"));
    al->addWidget(pileup_);
    lay->addWidget(alignGroup_);
    QHBoxLayout* tl = nullptr;
    tagsGroup_ = group(&tl);
    tl->addWidget(new QLabel(tr("Tags"), tagsGroup_));
    tagsEdit_ = new QLineEdit(tagsGroup_);
    tagsEdit_->setPlaceholderText(tr("NM,AS,RG"));
    tagsEdit_->setClearButtonEnabled(true);
    tagsEdit_->setMaximumWidth(140);
    tagsEdit_->setToolTip(tr("Add a typed column per optional tag of each record (comma-separated; "
                             "Enter applies)"));
    tl->addWidget(tagsEdit_);
    lay->addWidget(tagsGroup_);
    connect(tagsEdit_, &QLineEdit::returnPressed, this, [this] { apply(); });
    connect(tagsEdit_, &QLineEdit::textChanged, this, [this](const QString& t) {
        if (t.isEmpty() && !opts_.tags.empty()) apply();
    });

    // Variants: the sample columns' layout and genotype summaries.
    QHBoxLayout* sl = nullptr;
    samplesGroup_ = group(&sl);
    sl->addWidget(new QLabel(tr("Samples"), samplesGroup_));
    samples_ = segmented(samplesGroup_, sl, {"struct", "long", "text"},
                         {"One typed struct column per sample",
                          "One row per record × sample",
                          "The packed sample strings as in the file"});
    sl->setSpacing(0);
    lay->addWidget(samplesGroup_);
    connect(samples_, &QButtonGroup::idClicked, this, [this](int) { apply(); });
    gtStats_ = toggle(tr("GT stats"), tr("Per-variant genotype columns: het / hom / missing counts, "
                                         "AC / AN / AF, call rate"));
    lay->addWidget(gtStats_);
    expand_ = toggle(tr("Expand"), QString());
    lay->addWidget(expand_);
    contigs_ = toggle(tr("Contigs"), tr("List the reference sequences the header names (name, length) "
                                        "and the assembly, instead of the records"));
    lay->addWidget(contigs_);

    // Matrix tabs: one column per gene, or one row per stored value.
    QHBoxLayout* ml = nullptr;
    matrixGroup_ = group(&ml);
    ml->addWidget(new QLabel(tr("Matrix"), matrixGroup_));
    matrix_ = segmented(matrixGroup_, ml, {"wide", "long"},
                        {"Cells × genes, the first rows and columns (a preview of a large matrix)",
                         "One row per stored value: row, column, value — the whole matrix"});
    ml->setSpacing(0);
    lay->addWidget(matrixGroup_);
    connect(matrix_, &QButtonGroup::idClicked, this, [this](int) { apply(); });

    // NumPy arrays of 3+ axes: step the leading axis.
    QHBoxLayout* cl = nullptr;
    sliceGroup_ = group(&cl);
    cl->addWidget(new QLabel(tr("Slice"), sliceGroup_));
    for (int d : {-1, +1}) {
        auto* b = new QToolButton(sliceGroup_);
        b->setText(d < 0 ? QStringLiteral("◀") : QStringLiteral("▶"));
        b->setAutoRaise(true);
        b->setToolTip(d < 0 ? tr("The previous slice of the leading axis")
                            : tr("The next slice of the leading axis"));
        connect(b, &QToolButton::clicked, this, [this, d] { emit sliceStep(d); });
        cl->addWidget(b);
    }
    lay->addWidget(sliceGroup_);

    hint_ = new QLabel(this);
    hint_->setForegroundRole(QPalette::PlaceholderText);
    hint_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    hint_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);   // never widens the window
    lay->addWidget(hint_);
    error_ = new QLabel(this);
    error_->setWordWrap(false);
    error_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    error_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QPalette ep = error_->palette();
    ep.setColor(QPalette::WindowText, QColor(0xc0, 0x39, 0x2b));
    error_->setPalette(ep);
    lay->addWidget(error_);
    QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Preferred);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
    hide();
}

FileOptions ContextBar::current() const {
    FileOptions o = opts_;
    if (regionGroup_->isVisibleTo(this) && regionReady_) {
        o.region = regionEdit_->text().trimmed().toStdString();
        o.ncbi   = coords_->currentIndex() == 1;
        o.slop   = slop_->value();
    }
    if (alignGroup_->isVisibleTo(this)) o.pileup = pileup_->isChecked();
    if (tagsGroup_->isVisibleTo(this)) o.tags = tagsEdit_->text().trimmed().toStdString();
    if (samplesGroup_->isVisibleTo(this) && samples_->checkedId() >= 0)
        o.samples = kSampleLayouts[samples_->checkedId()];
    if (gtStats_->isVisibleTo(this)) o.gtStats = gtStats_->isChecked();
    if (expand_->isVisibleTo(this)) o.expand = expand_->isChecked();
    if (contigs_->isVisibleTo(this)) o.contigs = contigs_->isChecked();
    if (matrixGroup_->isVisibleTo(this) && matrix_->checkedId() >= 0)
        o.matrix = kMatrixLayouts[matrix_->checkedId()];
    return o;
}

void ContextBar::setState(const FileFeatures& file, const TabTraits& tab, const FileOptions& opts) {
    opts_ = opts;
    error_->clear();
    error_->setToolTip({});
    error_->hide();
    // Nothing emits while the controls take the file's options.
    const QSignalBlocker b1(regionEdit_), b2(coords_), b3(slop_), b4(pileup_), b5(tagsEdit_),
                         b6(samples_), b7(gtStats_), b8(expand_), b9(contigs_), b10(matrix_);

    FileFeatures::Region region = file.region;
    if (region == FileFeatures::Region::None && file.region_by_columns && tab.coord_columns)
        region = FileFeatures::Region::Ready;
    regionReady_ = region == FileFeatures::Region::Ready;
    regionGroup_->setVisible(region != FileFeatures::Region::None);
    regionEdit_->setText(QString::fromStdString(opts.region));
    coords_->setCurrentIndex(opts.ncbi ? 1 : 0);
    slop_->setValue((int)std::min<int64_t>(opts.slop, slop_->maximum()));
    alignGroup_->setVisible(file.pileup);
    pileup_->setChecked(opts.pileup);
    tagsGroup_->setVisible(file.tags);
    tagsEdit_->setText(QString::fromStdString(opts.tags));
    samplesGroup_->setVisible(file.has_samples);
    for (int i = 0; i < 3; ++i)
        if (opts.samples == kSampleLayouts[i]) samples_->button(i)->setChecked(true);
    gtStats_->setVisible(file.has_samples);
    gtStats_->setChecked(opts.gtStats);
    expand_->setVisible(!file.expand_column.empty());
    expand_->setText(tr("Expand %1").arg(QString::fromStdString(file.expand_column)));
    expand_->setToolTip(tr("One column per key of the packed %1 column (the column itself is kept)")
                            .arg(QString::fromStdString(file.expand_column)));
    expand_->setChecked(opts.expand);
    contigs_->setVisible(file.contigs);
    contigs_->setChecked(opts.contigs);
    matrixGroup_->setVisible(tab.matrix_tab);
    for (int i = 0; i < 2; ++i)
        if (opts.matrix == kMatrixLayouts[i]) matrix_->button(i)->setChecked(true);
    sliceGroup_->setVisible(tab.slices);

    // Options that do not combine are off rather than errors on apply.
    const QString byContigs = tr("Off while Contigs lists the header's reference sequences");
    const bool records = !opts.contigs;
    for (QWidget* w : std::initializer_list<QWidget*>{regionGroup_, alignGroup_, tagsGroup_, samplesGroup_,
                                                      gtStats_, expand_}) {
        w->setEnabled(records);
    }
    if (records && opts.pileup) tagsGroup_->setEnabled(false);
    tagsGroup_->setToolTip(!records ? byContigs
                           : opts.pileup ? tr("Off while Pileup shows per-base rows, not alignment records")
                                         : QString());
    alignGroup_->setToolTip(!records ? byContigs : QString());
    if (region == FileFeatures::Region::NeedsIndex) {
        for (QWidget* w : std::initializer_list<QWidget*>{regionEdit_, goButton_, coords_, slop_})
            w->setEnabled(false);
        regionGroup_->setToolTip(QString::fromStdString(file.region_hint));
    } else {
        for (QWidget* w : std::initializer_list<QWidget*>{regionEdit_, goButton_, coords_, slop_})
            w->setEnabled(true);
        regionGroup_->setToolTip(!records ? byContigs : QString());
    }
    // The region hint: how to index (always), or that a region reads the
    // whole file (while one is set).
    const bool showHint = region == FileFeatures::Region::NeedsIndex ||
                          (regionReady_ && !opts.region.empty());
    hint_->setText(showHint ? QString::fromStdString(file.region_hint) : QString());
    hint_->setVisible(!hint_->text().isEmpty());

    const bool any = regionGroup_->isVisibleTo(this) || alignGroup_->isVisibleTo(this) ||
                     tagsGroup_->isVisibleTo(this) || samplesGroup_->isVisibleTo(this) ||
                     gtStats_->isVisibleTo(this) || expand_->isVisibleTo(this) ||
                     contigs_->isVisibleTo(this) || matrixGroup_->isVisibleTo(this) ||
                     sliceGroup_->isVisibleTo(this);
    setVisible(any);
}

void ContextBar::clearState() {
    opts_ = FileOptions{};
    error_->clear();
    error_->hide();
    hide();
}

void ContextBar::showError(const QString& why, const FileOptions& attempted) {
    const QSignalBlocker b1(regionEdit_), b2(tagsEdit_);
    if (regionGroup_->isVisibleTo(this)) regionEdit_->setText(QString::fromStdString(attempted.region));
    if (tagsGroup_->isVisibleTo(this)) tagsEdit_->setText(QString::fromStdString(attempted.tags));
    error_->setText(fontMetrics().elidedText(tr("Not applied: %1").arg(why), Qt::ElideRight, 600));
    error_->setToolTip(why);
    error_->show();
}

void ContextBar::focusRegion() {
    if (!isVisible() || !regionGroup_->isVisible() || !regionEdit_->isEnabled()) return;
    regionEdit_->setFocus();
    regionEdit_->selectAll();
}

QString ContextBar::describeForTest() const {
    if (!isVisibleTo(parentWidget())) return QStringLiteral("none");
    QStringList parts;
    if (regionGroup_->isVisibleTo(this))
        parts << (regionEdit_->isEnabled() ? QStringLiteral("region") : QStringLiteral("region(needs-index)"));
    if (alignGroup_->isVisibleTo(this)) parts << QStringLiteral("pileup");
    if (tagsGroup_->isVisibleTo(this)) parts << QStringLiteral("tags");
    if (samplesGroup_->isVisibleTo(this)) parts << QStringLiteral("samples");
    if (gtStats_->isVisibleTo(this)) parts << QStringLiteral("gtstats");
    if (expand_->isVisibleTo(this)) parts << QStringLiteral("expand");
    if (contigs_->isVisibleTo(this)) parts << QStringLiteral("contigs");
    if (matrixGroup_->isVisibleTo(this)) parts << QStringLiteral("matrix");
    if (sliceGroup_->isVisibleTo(this)) parts << QStringLiteral("slices");
    return parts.join(QLatin1Char(' '));
}
