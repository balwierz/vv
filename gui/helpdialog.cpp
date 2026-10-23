#include "helpdialog.h"

#include <QDialogButtonBox>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QRegularExpression>
#include <QSplitter>
#include <QTextBrowser>
#include <QTextDocument>
#include <QVBoxLayout>

namespace {
const QString& reference_html() {
    static const QString html = [] {
        QFile f(QStringLiteral(":/help/reference.html"));
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    }();
    return html;
}
bool has_anchor(const QString& a) {
    return reference_html().contains(QStringLiteral("name=\"%1\"").arg(a));
}
// Each section: <a name="…"></a><h2>Title</h2>.
const QRegularExpression& section_re() {
    static const QRegularExpression re(QStringLiteral("<a name=\"([^\"]+)\"></a><h2>([^<]+)</h2>"));
    return re;
}
QPointer<HelpDialog> g_reference;
}   // namespace

QString HelpDialog::anchorFor(const QString& key_in) {
    QString key = key_in;
    if (const int dot = key.lastIndexOf(QLatin1Char('.')); dot >= 0) key = key.mid(dot + 1);   // "ClinVar.CLNSIG"
    QStringList tries{key};
    if (key.startsWith(QLatin1String("AS_"))) tries << key.mid(3);                   // allele-specific GATK keys
    if (const int us = key.indexOf(QLatin1Char('_')); us > 0) tries << key.left(us); // gnomAD: AF_nfe → AF
    for (const QString& k : tries)
        if (has_anchor(QStringLiteral("f-") + k)) return QStringLiteral("f-") + k;
    return {};
}

QStringList HelpDialog::sections() {
    QStringList out;
    for (auto it = section_re().globalMatch(reference_html()); it.hasNext();) out << it.next().captured(1);
    return out;
}

HelpDialog* HelpDialog::showReference(QWidget* parent, const QString& anchor) {
    if (!g_reference) g_reference = new HelpDialog(parent ? parent->window() : nullptr);
    g_reference->show();
    g_reference->raise();
    g_reference->activateWindow();
    if (!anchor.isEmpty()) g_reference->go(anchor);
    return g_reference;
}

HelpDialog::HelpDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Field Reference"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(900, 680);
    auto* v = new QVBoxLayout(this);
    auto* top = new QHBoxLayout;
    search_ = new QLineEdit(this);
    search_->setPlaceholderText(tr("Find: a key (CLNSIG, obsp, FLAG) or a word (strand bias)…"));
    search_->setClearButtonEnabled(true);
    status_ = new QLabel(this);
    status_->setForegroundRole(QPalette::PlaceholderText);
    top->addWidget(search_, 1);
    top->addWidget(status_);
    v->addLayout(top);
    auto* split = new QSplitter(this);
    sections_ = new QListWidget(split);
    text_ = new QTextBrowser(split);
    text_->setOpenExternalLinks(true);
    // Notes in the palette's secondary text colour, so a dark theme keeps
    // them readable.
    text_->document()->setDefaultStyleSheet(
        QStringLiteral("p.note { color: %1; }").arg(palette().color(QPalette::PlaceholderText).name()));
    text_->setHtml(reference_html());
    split->addWidget(sections_);
    split->addWidget(text_);
    split->setStretchFactor(1, 1);
    split->setSizes({200, 700});
    v->addWidget(split, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    v->addWidget(buttons);
    for (auto it = section_re().globalMatch(reference_html()); it.hasNext();) {
        const auto m = it.next();
        auto* item = new QListWidgetItem(m.captured(2), sections_);
        item->setData(Qt::UserRole, m.captured(1));
    }
    connect(sections_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
        if (item) text_->scrollToAnchor(item->data(Qt::UserRole).toString());
    });
    connect(search_, &QLineEdit::textChanged, this, [this] { find(false); });
    connect(search_, &QLineEdit::returnPressed, this, [this] { find(true); });
}

void HelpDialog::go(const QString& anchor) {
    text_->scrollToAnchor(anchor);
    // Select the key's text so the eye lands on it.
    const QString key = anchor.startsWith(QLatin1String("f-")) ? anchor.mid(2) : QString();
    if (!key.isEmpty()) {
        QTextCursor c = text_->document()->find(QRegularExpression(
            QStringLiteral("\\(%1\\)|\\b%1\\b").arg(QRegularExpression::escape(key))));
        if (!c.isNull()) {
            text_->setTextCursor(c);
            text_->scrollToAnchor(anchor);
        }
    }
}

void HelpDialog::find(bool next) {
    const QString what = search_->text().trimmed();
    if (what.isEmpty()) { status_->clear(); return; }
    QTextCursor from = next ? text_->textCursor() : QTextCursor(text_->document());
    QTextCursor hit = text_->document()->find(what, from);
    if (hit.isNull() && next) hit = text_->document()->find(what, QTextCursor(text_->document()));   // wrap
    if (hit.isNull()) { status_->setText(tr("not found")); return; }
    status_->setText(next ? tr("Enter: next") : QString());
    text_->setTextCursor(hit);
    text_->ensureCursorVisible();
}
