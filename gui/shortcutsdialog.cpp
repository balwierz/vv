#include "shortcutsdialog.h"

#include <QAction>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {
QPointer<ShortcutsDialog> g_shortcuts;
QString plain(const QAction* a) {
    QString n = a->text();
    n.remove(QLatin1Char('&'));
    return n;
}
}   // namespace

ShortcutsDialog* ShortcutsDialog::showFor(QWidget* parent, const Commands& commands) {
    if (!g_shortcuts) g_shortcuts = new ShortcutsDialog(commands, parent ? parent->window() : nullptr);
    g_shortcuts->commands_ = commands;
    g_shortcuts->fill();
    g_shortcuts->show();
    g_shortcuts->raise();
    g_shortcuts->activateWindow();
    g_shortcuts->filter_->setFocus();
    return g_shortcuts;
}

ShortcutsDialog::ShortcutsDialog(const Commands& commands, QWidget* parent)
    : QDialog(parent), commands_(commands) {
    setWindowTitle(tr("Keyboard Shortcuts"));
    setAttribute(Qt::WA_DeleteOnClose);
    resize(620, 600);
    auto* v = new QVBoxLayout(this);
    auto* top = new QHBoxLayout;
    filter_ = new QLineEdit(this);
    filter_->setPlaceholderText(tr("Filter: a command, a key or a menu (\"copy\", \"Ctrl\", \"View\")"));
    filter_->setClearButtonEnabled(true);
    auto* copy = new QPushButton(tr("Copy as Text"), this);
    top->addWidget(filter_, 1);
    top->addWidget(copy);
    v->addLayout(top);
    table_ = new QTableWidget(0, 3, this);
    table_->setHorizontalHeaderLabels({tr("Command"), tr("Shortcut"), tr("Where")});
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table_->verticalHeader()->hide();
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    v->addWidget(table_, 1);
    auto* note = new QLabel(tr("Also: click a column header to sort (ascending, descending, off); "
                               "right-click a header, a cell or a tab for its commands; "
                               "Enter in the Filter, Find and Region boxes applies them. "
                               "Help ▸ Field Reference (F1) has the syntax of the boxes."), this);
    note->setWordWrap(true);
    note->setForegroundRole(QPalette::PlaceholderText);
    v->addWidget(note);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::close);
    v->addWidget(buttons);
    connect(filter_, &QLineEdit::textChanged, this, [this] { fill(); });
    connect(copy, &QPushButton::clicked, this, [this] {
        QString text;
        for (int r = 0; r < table_->rowCount(); ++r) text += rowText(r) + QLatin1Char('\n');
        QGuiApplication::clipboard()->setText(text);
    });
    fill();
}

void ShortcutsDialog::fill() {
    const QString q = filter_->text().trimmed();
    table_->setRowCount(0);
    for (const auto& [where, a] : commands_) {
        const QString name = plain(a),
                      keys = QKeySequence::listToString(a->shortcuts(), QKeySequence::NativeText);
        if (!q.isEmpty() && !name.contains(q, Qt::CaseInsensitive) &&
            !keys.contains(q, Qt::CaseInsensitive) && !where.contains(q, Qt::CaseInsensitive))
            continue;
        const int r = table_->rowCount();
        table_->insertRow(r);
        table_->setItem(r, 0, new QTableWidgetItem(name));
        table_->setItem(r, 1, new QTableWidgetItem(keys));
        table_->setItem(r, 2, new QTableWidgetItem(where));
    }
}

int ShortcutsDialog::rowCount() const { return table_->rowCount(); }

QString ShortcutsDialog::rowText(int row) const {
    return table_->item(row, 0)->text() + QLatin1Char('\t') + table_->item(row, 1)->text() +
           QLatin1Char('\t') + table_->item(row, 2)->text();
}
